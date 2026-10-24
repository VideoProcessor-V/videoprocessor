#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "ConfigEditorWindow.h"
// VP-V section tests (VpvConfigTests.cpp), handed the suite's helpers.
int RunVpvConfigTests(const std::function<int(const char*, const std::function<void()>&)>& run,
    const std::function<void(bool, const char*)>& require, const std::function<void(ConfigEditorWindow&)>& save);
#include "../VideoProcessor-Config/UpdatePanel.h"
#include "ConfigEditorCore.h"
#include "../VideoProcessor-Config/ProfileListController.h"
#include "VpTheme.h"
#include <ActiveProfileStatus.h>
#include <ConfigurationLiveApply.h>
#include <ConfigurationIdentity.h>
#include <ConfigurationRpcProtocol.h>
#include <RendererProfileConfig.h>
#include <ConfigFile.h>
#include <led/LedDriveConfig.h>
#include <MainConfigSchema.h>
#include <QueueConfiguration.h>
#include <set>
#include <iterator>
#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <vector>
#include <cstring>
#include <RendererConfigView.h>
#include <RetiredSettings.h>
#include <ToneMappingTuning.h>

#include <QApplication>
#include <QAccessible>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QRadioButton>
#include <QFontDatabase>
#include <QColor>
#include "../VideoProcessor-Config/InlineColorPicker.h"
#include <QToolButton>
#include <QComboBox>
#include <QValidator>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QEventLoop>
#include <QFrame>
#include <QImage>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QListView>
#include <QMessageBox>
#include <QWheelEvent>
#include <cmath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QTabWidget>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QSlider>
#include <QStandardItemModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTextStream>
#include <QDoubleSpinBox>
#include <QThread>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
QString testNameFilter;
int selectedTestsRun = 0;
// Faster runs (owner 2026-10-15): --match a|b runs every test whose name
// contains one of the words (case-insensitive); --shard i/N runs every N-th
// test from i, so N copies run the suite in parallel. Tests that need the
// foreground window or the screen's z-order run only in shard 0, so parallel
// copies never take focus from each other.
QStringList testNameMatches;
int testShard = 0, testShardCount = 1, testSequence = 0;

WNDPROC testOwnerOriginalProcedure = nullptr;
HWND testAdvertisedEditor = nullptr;
bool testActivateOnAssociation = false;
bool testAssociationActivationAcknowledged = false;
uint32_t testPresentationTargetAcknowledgementSequence = 0;
HWND testPresentationTargetAcknowledgementEditor = nullptr;

void answerInputDialog(const QString& text);
void answerMessageBox(int result);

bool appearsAbove(HWND first, HWND second)
{
    for (HWND window = GetTopWindow(nullptr); window;
        window = GetWindow(window, GW_HWNDNEXT))
    {
        if (window == first) return true;
        if (window == second) return false;
    }
    return false;
}

struct ReverseCallbackFixture
{
    std::atomic<HWND> owner = nullptr;
    std::atomic<HWND> advertisedEditor = nullptr;
    std::atomic<bool> activationReturned = false;
    std::atomic<bool> activationAcknowledged = false;
    std::atomic<ULONGLONG> activationElapsedMs = 0;
};

struct ExternalZOrderFixture
{
    HWND owner = nullptr;
    HWND host = nullptr;
    bool hostBecameForeground = false;
};

LRESULT CALLBACK externalFixtureReceiverProcedure(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    auto* fixture = reinterpret_cast<ExternalZOrderFixture*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        fixture = static_cast<ExternalZOrderFixture*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(fixture));
    }
    static const UINT readyMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigTests.ZOrderFixture.Ready.v1");
    static const UINT foregroundMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigTests.ZOrderFixture.Foreground.v1");
    if (fixture && message == readyMessage)
    {
        fixture->owner = reinterpret_cast<HWND>(wParam);
        fixture->host = reinterpret_cast<HWND>(lParam);
        return 1;
    }
    if (fixture && message == foregroundMessage)
    {
        fixture->hostBecameForeground = wParam != 0;
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct ExternalHostContext { HWND receiver = nullptr; };

BOOL CALLBACK collectMonitorRects(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info))
        reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcMonitor);
    return TRUE;
}

LRESULT CALLBACK externalHostProcedure(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    auto* context = reinterpret_cast<ExternalHostContext*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        context = static_cast<ExternalHostContext*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(context));
    }
    static const UINT requestMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigTests.ZOrderFixture.RequestForeground.v1");
    if (context && message == requestMessage)
    {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        const bool foregrounded = SetForegroundWindow(window) != FALSE &&
            GetForegroundWindow() == window;
        static const UINT foregroundMessage = RegisterWindowMessageW(
            L"VideoProcessor.ConfigTests.ZOrderFixture.Foreground.v1");
        PostMessageW(context->receiver, foregroundMessage, foregrounded, 0);
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int runExternalZOrderFixture(HWND receiver)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = externalHostProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"VP.ConfigTests.ExternalHost.v1";
    RegisterClassW(&windowClass);
    ExternalHostContext context{ receiver };
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName,
        L"VP external owner", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
        nullptr, nullptr, instance, &context);
    std::vector<RECT> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitorRects,
        reinterpret_cast<LPARAM>(&monitors));
    RECT bounds{};
    if (!monitors.empty()) bounds = monitors[monitors.size() > 1 ? 1 : 0];
    else
    {
        bounds.right = GetSystemMetrics(SM_CXSCREEN);
        bounds.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    HWND host = CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName,
        L"VP external fullscreen host", WS_POPUP,
        bounds.left, bounds.top, bounds.right - bounds.left,
        bounds.bottom - bounds.top, nullptr, nullptr, instance, &context);
    if (!owner || !host) return 2;
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(host, HWND_TOPMOST, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    static const UINT readyMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigTests.ZOrderFixture.Ready.v1");
    PostMessageW(receiver, readyMessage, reinterpret_cast<WPARAM>(owner),
        reinterpret_cast<LPARAM>(host));
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(host);
    DestroyWindow(owner);
    return 0;
}

LRESULT CALLBACK reverseCallbackOwnerProcedure(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    ReverseCallbackFixture* fixture = reinterpret_cast<ReverseCallbackFixture*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        fixture = static_cast<ReverseCallbackFixture*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(fixture));
    }
    static const UINT associationMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.Association.v1");
    if (fixture && message == associationMessage)
    {
        const HWND editor = reinterpret_cast<HWND>(lParam);
        fixture->advertisedEditor.store(editor);
        static const UINT activationMessage = RegisterWindowMessageW(
            L"VideoProcessor.ConfigEditor.Activate.v1");
        DWORD_PTR acknowledged = 0;
        const ULONGLONG start = GetTickCount64();
        const bool delivered = activationMessage && SendMessageTimeoutW(editor,
            activationMessage, wParam, reinterpret_cast<LPARAM>(window),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledged);
        fixture->activationElapsedMs.store(GetTickCount64() - start);
        fixture->activationAcknowledged.store(delivered && acknowledged == 1);
        fixture->activationReturned.store(true);
        return 1;
    }
    if (message == WM_CLOSE)
    {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK testOwnerProcedure(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    static const UINT associationMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.Association.v1");
	static const UINT presentationTargetAcknowledgementMessage =
		RegisterWindowMessageW(
			L"VideoProcessor.ConfigEditor.PresentationTargetAck.v2");
	if (message == presentationTargetAcknowledgementMessage)
	{
		testPresentationTargetAcknowledgementSequence =
			static_cast<uint32_t>(wParam);
		testPresentationTargetAcknowledgementEditor =
			reinterpret_cast<HWND>(lParam);
		return 1;
	}
    if (message == associationMessage)
    {
        const HWND editor = reinterpret_cast<HWND>(lParam);
        DWORD processId = 0;
        if (editor && IsWindow(editor))
            GetWindowThreadProcessId(editor, &processId);
        if (processId == static_cast<DWORD>(wParam))
        {
            testAdvertisedEditor = editor;
            if (testActivateOnAssociation)
            {
                testActivateOnAssociation = false;
                static const UINT activationMessage = RegisterWindowMessageW(
                    L"VideoProcessor.ConfigEditor.Activate.v1");
                DWORD_PTR acknowledged = 0;
                testAssociationActivationAcknowledged = activationMessage &&
                    SendMessageTimeoutW(editor, activationMessage, wParam,
                        reinterpret_cast<LPARAM>(window),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledged) &&
                    acknowledged == 1;
            }
            return 1;
        }
        return 0;
    }
    return testOwnerOriginalProcedure ? CallWindowProcW(
        testOwnerOriginalProcedure, window, message, wParam, lParam) :
        DefWindowProcW(window, message, wParam, lParam);
}

QString repositoryPath(const QString& relative)
{
    QString source = QString::fromUtf8(__FILE__);
    source.replace(u'\\', u'/');
    const QString marker = QStringLiteral("/src/VideoProcessor-ConfigTests/ConfigEditorWindowTests.cpp");
    const qsizetype position = source.lastIndexOf(marker);
    if (position < 0) throw std::runtime_error("Cannot locate repository root");
    return source.left(position) + u'/' + relative;
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read test file");
    return file.readAll();
}

QString copyFixture(QTemporaryDir& directory)
{
    if (!directory.isValid()) throw std::runtime_error("Cannot create temporary directory");
    const QString destination = directory.filePath(QStringLiteral("VideoProcessor.cfg"));
    if (!QFile::copy(repositoryPath(QStringLiteral(
        "test-fixtures/deployed-VideoProcessor-20260807-current.cfg")), destination))
        throw std::runtime_error("Cannot copy deployed configuration fixture");

    // The checked-in fixture follows the repository's Windows line-ending
    // policy. These UI tests inspect precise multi-line snippets, so give each
    // disposable copy a stable line ending without changing the source fixture
    // or weakening the editor's separate preservation tests.
    QByteArray contents = readBytes(destination);
    contents.replace("\r\n", "\n");
    QFile normalized(destination);
    if (!normalized.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        normalized.write(contents) != contents.size())
        throw std::runtime_error("Cannot normalize configuration fixture");
    normalized.close();
    return destination;
}

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
T* requireControl(QObject& root, const QString& name)
{
    T* control = root.findChild<T*>(name);
    if (!control)
        throw std::runtime_error(QStringLiteral("Missing control: %1").arg(name).toStdString());
    return control;
}

void selectData(QComboBox* combo, const QString& value)
{
    int index = combo->findData(value, Qt::UserRole, Qt::MatchFixedString);
    if (index < 0)
    {
        combo->addItem(value, value);
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(index);
    if (combo->isEditable()) combo->setEditText(value);
    QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection,
        Q_ARG(int, index));
    QCoreApplication::processEvents();
}

void save(ConfigEditorWindow& window)
{
    QPushButton* button = requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"));
    require(button->isEnabled(), "Apply button was not enabled after an edit");
    button->click();
    QCoreApplication::processEvents();
    const QString status = requireControl<QLabel>(window, QStringLiteral("configurationStatus"))->text();
    require(status.startsWith(QStringLiteral("Changes saved")) ||
        status.startsWith(QStringLiteral("Configuration created safely")),
        (std::string("Apply did not save: ") + status.toStdString()).c_str());
    require(!button->isEnabled(), "Apply did not complete successfully");
}

void testRendererCardsShowDefaultsAndRoundTrip()
{
    // Audit 2026-10-14c: unset settings show the renderer's real defaults,
    // only what applies is shown, and Apply / reopen keeps every edit while
    // the rest of the file stays as it was.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    const QByteArray original = "# a comment that stays\n[vprenderer.Cinema]\nquality: high # kept\n";
    {
        QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
        file.write(original);
    }
    auto rowShown = [](QWidget* field)
    {
        auto* layout = field->parentWidget() ? qobject_cast<QFormLayout*>(field->parentWidget()->layout()) : nullptr;
        return layout && layout->isRowVisible(field);
    };
    {
        ConfigEditorWindow window(path, 0, true);
        requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
        QCoreApplication::processEvents();
        auto* tolerance = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_match_tolerance");
        require(tolerance->text().isEmpty() && tolerance->placeholderText() == "4.5",
            ("An unset match tolerance does not show its default 4.5: '" + tolerance->text().toStdString() + "'").c_str());
        auto* fade = requireControl<QComboBox>(window, "config.vprenderer.dejudder_fade_match");
        require(fade->currentIndex() == 0 && fade->itemText(0).contains("On", Qt::CaseInsensitive),
            ("An unset fade match does not say its default (on): " + fade->itemText(0).toStdString()).c_str());
        auto* capture = requireControl<QComboBox>(window, "config.vprenderer.capture_path");
        // GPU unpack is the default on both paths (owner 2026-10-15).
        require(capture->itemText(0).contains("gpu", Qt::CaseInsensitive),
            ("An unset capture path does not say its default (gpu): " + capture->itemText(0).toStdString()).c_str());
        // The de-judder: with no strength only the strengths show.
        auto* strength = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_strength");
        auto* rolloff = requireControl<QComboBox>(window, "config.vprenderer.dejudder_motion_rolloff");
        auto* rollLo = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_roll_lo");
        require(!rowShown(rolloff), "Strength shaping shows with the de-judder off");
        strength->setText("0.25");
        require(rowShown(rolloff) && !rowShown(rollLo), "With a strength: the strobing switch, not its values (it is off)");
        selectData(rolloff, "on");
        require(rowShown(rollLo), "The strobing values do not show with the switch on");
        // Dependent rows of the measured section.
        require(!rowShown(requireControl<QLineEdit>(window, "config.vprenderer.dejudder_est_split_sad")),
            "The split threshold shows with splitting off (the default)");
        // A "from" above its "to" is flagged.
        requireControl<QLineEdit>(window, "config.vprenderer.dejudder_motion_scale_lo")->setText("2");
        auto* motionHi = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_motion_scale_hi");
        motionHi->setText("1");
        require(motionHi->property("pairWarning").toBool(), "A motion scale 'to' below its 'from' is not flagged");
        motionHi->setText("");
        requireControl<QLineEdit>(window, "config.vprenderer.dejudder_motion_scale_lo")->setText("");
        // SDR: the knee's own rows only with the knee.
        auto* shape = requireControl<QComboBox>(window, "config.vprenderer.sdr_tone_shape");
        auto* kneeStart = requireControl<QLineEdit>(window, "config.vprenderer.sdr_knee_start");
        require(!kneeStart->isEnabled(), "The knee start is offered with the SDR shape off");
        selectData(shape, "knee");
        require(kneeStart->isEnabled(), "The knee start is not offered with the knee");
        selectData(capture, "gpu");
        selectData(fade, "off");
        save(window);
    }
    {
        ConfigEditorWindow window(path, 0, true);
        requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
        QCoreApplication::processEvents();
        require(requireControl<QLineEdit>(window, "config.vprenderer.dejudder_strength")->text() == "0.25",
            "The strength did not survive Apply and reopen");
        require(requireControl<QComboBox>(window, "config.vprenderer.capture_path")->currentData().toString() == "gpu",
            "The capture path did not survive Apply and reopen");
        require(requireControl<QComboBox>(window, "config.vprenderer.sdr_tone_shape")->currentData().toString() == "knee",
            "The SDR shape did not survive Apply and reopen");
        require(requireControl<QComboBox>(window, "config.vprenderer.dejudder_fade_match")->currentData().toString() == "off",
            "The fade match did not survive Apply and reopen");
        require(requireControl<QComboBox>(window, "config.vprenderer.dejudder_motion_rolloff")->currentData().toString() == "on",
            "The strobing switch did not survive Apply and reopen");
    }
    const QByteArray saved = readBytes(path);
    for (const char* kept : { "# a comment that stays", "quality: high # kept" })
        require(saved.contains(kept), (QByteArray("A line outside the edits changed: ") + kept).constData());
    for (const char* edited : { "dejudder_strength: 0.25", "dejudder_motion_rolloff: on", "sdr_tone_shape: knee",
            "capture_path: gpu", "dejudder_fade_match: off" })
        require(saved.contains(edited), (QByteArray("An edit was not saved: ") + edited).constData());
    // Only the edited renderer keys were written (no unset default saved).
    int rendererKeys = 0;
    for (const QByteArray& line : saved.split('\n'))
        if (line.startsWith("dejudder_") || line.startsWith("sdr_") || line.startsWith("capture_") ||
                line.startsWith("adaptive_") || line.startsWith("dither_") || line.startsWith("occlusion_"))
            ++rendererKeys;
    require(rendererKeys == 5, ("Renderer keys other than the five edits were written:\n" + saved).constData());
}

// Saves the window's current page as a PNG in %VP_TEST_SHOTS% (when set),
// for a person to look at the layout.
void shoot(ConfigEditorWindow& window, const char* name)
{
    const QString folder = qEnvironmentVariable("VP_TEST_SHOTS");
    if (folder.isEmpty()) return;
    for (int pass = 0; pass < 5; ++pass)   // let opened sections lay out
    {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    window.grab().save(QDir(folder).filePath(QString::fromLatin1(name) + QStringLiteral(".png")));
}

void testEveryNewLedControlReachesVp()
{
    // Owner 2026-10-14: every new Config control does what it says - set it,
    // Apply, and VP's own reader gets the value.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    {
        QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
        file.write("[led_drive.Default]\nenabled: false\n[projector.Default]\nport: COM1\n");
    }
    {
        ConfigEditorWindow window(path, 0, true);
        window.resize(1500, 1150);
        window.show();
        window.selectPage(20);
        shoot(window, "led-new-file");
        auto* enabled = requireControl<QCheckBox>(window, "config.led_drive.default.enabled");
        enabled->setChecked(true);
        auto* source = requireControl<QComboBox>(window, "config.led_drive.default.command_source");
        auto* protocol = requireControl<QComboBox>(window, "config.projector.default.protocol");
        require(protocol->currentData().toString() == "none", "A new file's protocol does not show no projector");
        selectData(source, "internal_script");
        require(protocol->currentData().toString() == "barco_ascii", "Picking VP's copy of the script did not switch to Barco");
        // Script pacing (owner 2026-10-15): the script's rhythm by default, with its two times.
        require(requireControl<QComboBox>(window, "config.led_drive.default.script_pacing")->currentData().toString() == "script",
            "Script pacing does not show the script's rhythm as the default");
        requireControl<QSpinBox>(window, "config.led_drive.default.script_measure_ms")->setValue(120);
        requireControl<QSpinBox>(window, "config.led_drive.default.script_echo_ms")->setValue(40);
        requireControl<QCheckBox>(window, "config.led_drive.default.script_lookahead")->setChecked(false);
        shoot(window, "led-internal-script");
        requireControl<QCheckBox>(window, "config.led_drive.default.adaptive_gamma")->setChecked(true);
        selectData(source, "compensate");
        requireControl<QCheckBox>(window, "config.led_drive.default.lookahead")->setChecked(false);
        auto setNumber = [&](const char* name, double value)
        {
            requireControl<QDoubleSpinBox>(window, name)->setValue(value);
        };
        requireControl<QToolButton>(window, "config.projector.default.advanced_toggle")->setChecked(true);
        setNumber("config.projector.default.baud", 38400);
        setNumber("config.projector.default.light_min", 60);
        setNumber("config.projector.default.light_max", 250);
        setNumber("config.projector.default.ack_timeout_ms", 400);
        setNumber("config.projector.default.min_command_gap_ms", 150);
        setNumber("config.projector.default.max_levels_per_command", 10);
        setNumber("config.projector.default.burst_commands", 5);
        setNumber("config.projector.default.burst_pause_ms", 3000);
        requireControl<QCheckBox>(window, "config.projector.default.echo")->setChecked(false);
        selectData(requireControl<QComboBox>(window, "config.projector.default.terminator"), "literal_cr");
        requireControl<QLineEdit>(window, "config.projector.default.flux_table")->setText("60:0.21,255:1");
        requireControl<QSpinBox>(window, "config.projector.default.led_latency_ms")->setValue(1200);
        shoot(window, "led-compensate");
        save(window);
    }
    ConfigFile config;
    require(config.Load(path.toStdString()), "The saved LED file did not load");
    std::string error;
    require(LedDrive::Validate(config, error), ("VP rejects the saved LED settings: " + error).c_str());
    const LedDrive::LedDriveConfig led = LedDrive::ReadLedDriveConfig(config);
    const LedDrive::ProjectorConfig projector = LedDrive::ReadProjectorConfig(config);
    require(led.enabled && led.mode == LedDrive::Mode::Compensate && led.commandSource == "compensate",
        "VP does not read Compensate as the source");
    require(led.adaptiveGamma && !led.lookahead && led.scriptPacing && led.scriptMeasureMs == 120 && led.scriptEchoMs == 40 &&
        !led.scriptLookahead, "VP does not read adaptive gamma / look-ahead / script pacing / script look-ahead as set");
    require(projector.protocol == "barco_ascii" && projector.baud == 38400 && projector.lightMin == 60 &&
        projector.lightMax == 250 && projector.ackTimeoutMs == 400 && projector.minCommandGapMs == 150 &&
        projector.maxLevelsPerCommand == 10 && projector.burstCommands == 5 && projector.burstPauseMs == 3000 &&
        !projector.echo && projector.terminator == LedDrive::Terminator::LiteralCr && projector.fluxTable.size() == 2 &&
        projector.ledLatencyMs == 1200, "VP does not read every projector setting as set");
    {
        // Reopened: each control shows what was saved.
        ConfigEditorWindow window(path, 0, true);
        window.selectPage(20);
        require(requireControl<QComboBox>(window, "config.led_drive.default.command_source")->currentData().toString() == "compensate" &&
            requireControl<QCheckBox>(window, "config.led_drive.default.adaptive_gamma")->isChecked() &&
            !requireControl<QCheckBox>(window, "config.led_drive.default.lookahead")->isChecked() &&
            !requireControl<QCheckBox>(window, "config.led_drive.default.script_lookahead")->isChecked() &&
            requireControl<QSpinBox>(window, "config.led_drive.default.script_measure_ms")->value() == 120 &&
            requireControl<QDoubleSpinBox>(window, "config.projector.default.baud")->value() == 38400 &&
            requireControl<QDoubleSpinBox>(window, "config.projector.default.light_min")->value() == 60 &&
            !requireControl<QCheckBox>(window, "config.projector.default.echo")->isChecked() &&
            requireControl<QComboBox>(window, "config.projector.default.terminator")->currentData().toString() == "literal_cr" &&
            requireControl<QSpinBox>(window, "config.projector.default.led_latency_ms")->value() == 1200,
            "A reopened Config does not show the saved LED settings");
    }
    // "Ask the projector" removes the key again (VP's default).
    {
        ConfigEditorWindow window(path, 0, true);
        window.selectPage(20);
        auto* lightMin = requireControl<QDoubleSpinBox>(window, "config.projector.default.light_min");
        lightMin->setValue(lightMin->minimum());
        save(window);
    }
    require(!readBytes(path).contains("light_min"), "Ask the projector did not remove light_min");
}

void testEveryNewRendererControlReachesVp()
{
    // Owner 2026-10-14: every new or moved renderer control writes the key
    // and value VP reads.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    {
        QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
        file.write("[vprenderer.Cinema]\nquality: high\n");
    }
    struct Edit { const char* key; const char* value; bool choice; };
    const Edit edits[] = {
        { "capture_path", "gpu", true }, { "dejudder_strength", "0.25", false }, { "dejudder_strength_even", "0.5", false },
        { "dejudder_even_rates", "whole_and_1001", true }, { "dejudder_motion_rolloff", "on", true },
        { "dejudder_roll_lo", "6", false }, { "dejudder_roll_hi", "20", false }, { "dejudder_roll_floor", "0.3", false },
        { "dejudder_object_strength", "0.5", false }, { "dejudder_fade_floor", "0.2", false },
        { "dejudder_fade_match", "off", true }, { "dejudder_match_tolerance", "5", false },
        { "dejudder_frame_trust", "0.3", false }, { "dejudder_est_gate", "off", true },
        { "dejudder_global_prior", "off", true }, { "dejudder_vector_smooth", "off", true },
        { "dejudder_motion_scale_lo", "0.5", false }, { "dejudder_motion_scale_hi", "2", false },
        { "dejudder_trust_scale", "0.5", false }, { "dejudder_est_lambda", "1", false },
        { "dejudder_est_candidates", "off", true }, { "dejudder_est_qpel", "off", true },
        { "dejudder_est_split_sad", "6", false }, { "dejudder_est_split", "on", true },
        { "dejudder_search_range", "6", false }, { "dejudder_global_two", "off", true },
        { "dejudder_texture_tolerance", "0.5", false }, { "dejudder_consistency_px", "3", false },
        { "dejudder_cut_histogram", "0.4", false }, { "dejudder_regional_hold", "off", true },
        { "dejudder_pixel_trust_lo", "2", false }, { "dejudder_pixel_trust_hi", "7", false },
        { "dejudder_landing_check", "off", true }, { "dejudder_pixel_trust", "off", true },
        { "dejudder_pulldown", "on", true },
        { "dv_tunnel_gpu_unpack", "off", true }, { "dv_trims", "off", true },
        { "dejudder_verify_share", "0.3", false },
        { "dejudder_verify", "on", true }, { "dejudder_low_res", "search", true },
        { "dejudder_estimate_ahead", "off", true }, { "dejudder_hold_feather_px", "3", false },
        { "dejudder_hold_hysteresis", "3", false }, { "dejudder_taper_sad", "9", false },
        { "dejudder_block_taper", "on", true }, { "dejudder_warp_stage", "source", true },
        { "dejudder_occlusion_fill", "on", true }, { "occlusion_mask", "on", true },
        { "dither_grain_tolerance_scale", "2", false }, { "dither_vectors", "own", true },
        { "sdr_tone_shape", "knee", true }, { "sdr_white_headroom", "0.1", false }, { "sdr_knee_start", "0.7", false },
        { "sdr_knee_strength", "0.6", false }, { "sdr_contrast_recovery", "1.2", false },
        { "adaptive_downscale", "on", true }, { "adaptive_upscale", "on", true }, { "upscale_light", "gamma", true },
        { "fbo_float32", "on", true }, { "queue_timing_fixes", "off", true }, { "efficiency_fixes", "off", true } };
    {
        ConfigEditorWindow window(path, 0, true);
        window.resize(1500, 1600);
        window.show();
        window.selectPage(2);
        requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
        QCoreApplication::processEvents();
        for (const char* section : { "rendererSection.dejudder", "rendererSection.dejudderEstimation" })
            if (auto* toggle = window.findChild<QToolButton*>(QString::fromLatin1(section))) toggle->setChecked(true);
        shoot(window, "dejudder-off");
        for (const Edit& edit : edits)
        {
            const QString name = QStringLiteral("config.vprenderer.") + QString::fromLatin1(edit.key);
            if (edit.choice) selectData(requireControl<QComboBox>(window, name), QString::fromLatin1(edit.value));
            else requireControl<QLineEdit>(window, name)->setText(QString::fromLatin1(edit.value));
        }
        shoot(window, "dejudder-set");
        // The frame-rate rule on a rendering profile.
        requireControl<QDoubleSpinBox>(window, "config.vprenderer.rate_rule_fps")->setValue(59.0);
        requireControl<QCheckBox>(window, "config.vprenderer.rate_rule")->setChecked(true);
        save(window);
    }
    ConfigFile config;
    require(config.Load(path.toStdString()), "The saved renderer file did not load");
    RendererProfileConfig::Model model;
    std::string error;
    require(RendererProfileConfig::Read(config, model, error), ("VP rejects the saved renderer settings: " + error).c_str());
    const RendererProfileConfig::Profile* cinema = nullptr;
    std::string names;
    for (const auto& entry : model.profiles)
    {
        names += entry.first + " ";
        if (entry.second.settings.count("quality")) cinema = &entry.second;
    }
    require(cinema != nullptr, ("The Cinema profile is missing after the save; profiles: " + names).c_str());
    for (const Edit& edit : edits)
    {
        const auto found = cinema->settings.find(edit.key);
        require(found != cinema->settings.end() && found->second == edit.value,
            (std::string("VP does not read ") + edit.key + " = " + edit.value + " (got '" +
                (found == cinema->settings.end() ? std::string("unset") : found->second) + "')").c_str());
    }
    require(cinema->when == "${cadence} > 59", ("The frame-rate rule is not the profile's: " + cinema->when).c_str());
}

void testConfigKeysActivateTheRendererAndMatcherSettings()
{
    // Owner 2026-10-14: every Config key reaches the setting the
    // renderer and the de-judder matcher actually use - through the
    // renderer DLL's own loader and the same profile-to-matcher
    // mapping RunDejudderEstimate uses (no GPU needed).
    const std::string folder = QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toStdString() + "\\";
    // The VP renderer, or its Beta build (the same source and settings code)
    // when Windows refuses to load a fresh build of the first.
    std::string dllPath = folder + "vprenderer\\VideoProcessorVPRenderer.dll";
    HMODULE dll = LoadLibraryExA(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll)
    {
        std::cout << "  " << dllPath << " not loadable (error " << GetLastError() << "); trying the Beta build\n";
        dllPath = folder + "vprenderer-beta\\VideoProcessorVPRendererBeta.dll";
        dll = LoadLibraryExA(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    require(dll != nullptr, ("cannot load " + dllPath).c_str());
    std::cout << "  settings described by " << dllPath << "\n";
    using Describe = int(__cdecl*)(const char*, char*, int);
    const auto describe = reinterpret_cast<Describe>(GetProcAddress(dll, "VideoProcessorLibplaceboDescribeSettingsForTest"));
    require(describe != nullptr, "the renderer DLL has no settings description");
    auto read = [&](const std::string& body)
    {
        const std::string path = folder + "vp-settings-activation.cfg";
        // The base rendering profile (every real configuration has it; a file
        // with only a named profile selects none).
        std::ofstream(path, std::ios::trunc) << "[vprenderer]\nquality: high\n" << body;
        std::vector<char> text(16384);
        require(describe(path.c_str(), text.data(), static_cast<int>(text.size())) > 0, "the renderer could not describe the settings");
        std::map<std::string, std::string> values;
        std::istringstream in(text.data());
        for (std::string line; std::getline(in, line);)
        {
            const size_t equals = line.find('=');
            if (equals != std::string::npos) values[line.substr(0, equals)] = line.substr(equals + 1);
        }
        return values;
    };
    // Every key set away from its default (as the Config test does).
    const std::map<std::string, std::string> set = read(
        "capture_path: gpu\nsource_range: limited\nsource_resolution: 900\ndejudder_strength: 0.25\ndejudder_strength_even: 0.5\ndejudder_even_rates: whole_and_1001\n"
        "dejudder_motion_rolloff: on\ndejudder_roll_lo: 6\ndejudder_roll_hi: 20\ndejudder_roll_floor: 0.3\n"
        "dejudder_object_strength: 0.5\ndejudder_fade_floor: 0.2\ndejudder_fade_match: off\n"
        "dejudder_match_tolerance: 5\ndejudder_frame_trust: 0.3\ndejudder_est_gate: off\ndejudder_global_prior: off\n"
        "dejudder_vector_smooth: off\ndejudder_motion_scale_lo: 0.5\ndejudder_motion_scale_hi: 2\n"
        "dejudder_trust_scale: 0.5\ndejudder_est_lambda: 1\ndejudder_est_candidates: off\ndejudder_est_qpel: off\n"
        "dejudder_est_split_sad: 6\ndejudder_est_split: on\ndejudder_search_range: 6\ndejudder_global_two: off\n"
        "dejudder_texture_tolerance: 0.5\ndejudder_consistency_px: 3\ndejudder_cut_histogram: 0.4\n"
        "dejudder_regional_hold: off\ndejudder_pixel_trust_lo: 2\ndejudder_pixel_trust_hi: 7\ndejudder_pixel_trust: off\ndejudder_landing_check: off\ndejudder_pulldown: on\n"
        "dejudder_verify_share: 0.3\ndejudder_verify: on\ndejudder_low_res: search\ndejudder_estimate_ahead: off\n"
        "dejudder_hold_feather_px: 3\ndejudder_hold_hysteresis: 3\ndejudder_taper_sad: 9\ndejudder_block_taper: on\n"
        "dejudder_warp_stage: source\ndejudder_occlusion_fill: on\nocclusion_mask: on\n"
        "dither_grain_tolerance_scale: 2\ndither_vectors: own\nsdr_tone_shape: knee\nsdr_white_headroom: 0.1\n"
        "sdr_knee_start: 0.7\nsdr_knee_strength: 0.6\nsdr_contrast_recovery: 1.2\nadaptive_downscale: on\n"
        "adaptive_upscale: on\nupscale_light: gamma\nfbo_float32: on\n");
    const std::pair<const char*, const char*> expected[] = {
        { "capture_path", "gpu" }, { "source_range", "limited" }, { "source_resolution", "900" }, { "dejudder_strength", "0.25" }, { "dejudder_strength_even", "0.5" },
        { "dejudder_even_includes_1001", "1" }, { "dejudder_warp_rendered", "0" }, { "dejudder_estimate_ahead", "0" },
        { "occlusion_mask", "1" }, { "dither_vectors_own", "1" }, { "dither_grain_tolerance_scale", "2" },
        { "sdr_knee", "1" }, { "sdr_headroom", "0.1" }, { "sdr_knee_start", "0.7" }, { "sdr_knee_strength", "0.6" },
        { "sdr_recovery", "1.2" }, { "adaptive_downscale", "1" }, { "adaptive_upscale", "1" },
        { "upscale_light", "gamma" }, { "fbo_float32", "1" },
        { "matcher.matchTolerance", "5" }, { "matcher.minTrustedShare", "0.3" }, { "matcher.globalPrior", "0" },
        { "matcher.vectorSmooth", "0" }, { "matcher.motionScaleLo", "0.5" }, { "matcher.motionScaleHi", "2" },
        { "matcher.trustScaleFull", "0.5" }, { "matcher.occlusionFill", "1" }, { "matcher.estCandidates", "0" },
        { "matcher.estLambda", "1" }, { "matcher.estQpel", "0" }, { "matcher.estGate", "0" }, { "matcher.estSplit", "1" },
        { "matcher.estSplitSad", "6" }, { "matcher.motionRolloff", "1" }, { "matcher.rollLo", "6" },
        { "matcher.rollHi", "20" }, { "matcher.rollFloor", "0.3" }, { "matcher.objectStrength", "0.5" },
        { "matcher.textureTolerance", "0.5" }, { "matcher.globalTwo", "0" }, { "matcher.regionalHold", "0" },
        { "matcher.cutHistogram", "0.4" }, { "matcher.coarseRange", "6" }, { "matcher.consistencyPixels", "3" },
        { "matcher.fadeMatch", "0" }, { "matcher.fadeFloor", "0.2" }, { "matcher.verifyPixels", "1" },
        { "matcher.verifyShare", "0.3" }, { "matcher.pixelTrust", "0" }, { "matcher.pixelTrustLo", "2" },
        { "matcher.pixelTrustHi", "7" }, { "matcher.landingCheck", "0" }, { "dejudder_pulldown", "1" }, { "matcher.lowRes", "1" }, { "matcher.lowResAll", "0" },
        { "matcher.holdFeatherPx", "3" }, { "matcher.holdHysteresis", "3" }, { "matcher.blockTaper", "1" },
        { "matcher.taperSad", "9" } };
    for (const auto& entry : expected)
    {
        const auto found = set.find(entry.first);
        const std::string got = found == set.end() ? std::string("(missing)") : found->second;
        require(got == entry.second, (std::string("configured ") + entry.first + " not active: got " + got + ", want " + entry.second).c_str());
    }
    // Unset: the defaults Config now shows (rendered path, fade match
    // and pixel trust on, split off, half size every step, standard).
    const std::map<std::string, std::string> unset = read("");
    const std::pair<const char*, const char*> defaults[] = {
        { "capture_path", "gpu" }, { "source_range", "auto" }, { "source_resolution", "auto" }, { "dejudder_warp_rendered", "1" }, { "matcher.fadeMatch", "1" },
        { "matcher.pixelTrust", "1" }, { "matcher.landingCheck", "1.5" }, { "matcher.pictureArea", "1" }, { "dejudder_pulldown", "0" }, { "matcher.estSplit", "0" }, { "matcher.lowResAll", "1" },
        { "matcher.matchTolerance", "4.5" }, { "matcher.textureTolerance", "0.25" }, { "matcher.globalTwo", "1" },
        { "matcher.regionalHold", "1" }, { "matcher.verifyPixels", "0" }, { "matcher.motionRolloff", "0" },
        { "sdr_knee", "0" }, { "adaptive_downscale", "0" }, { "occlusion_mask", "0" } };
    for (const auto& entry : defaults)
    {
        const auto found = unset.find(entry.first);
        { const std::string got = found == unset.end() ? std::string("(missing)") : found->second; require(got == entry.second, (std::string("the default of ") + entry.first + " differs from what Config shows: " + got).c_str()); }
    }
    // Occlusion fill is ignored with the rendered warp (as Config greys it).
    require(read("dejudder_occlusion_fill: on\n")["matcher.occlusionFill"] == "0",
        "occlusion fill reaches the matcher with the rendered warp");
    FreeLibrary(dll);
        }

void testSubtitleDiagnosticSurvivesConfigSaveAndReopen()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "Cannot create subtitle diagnostic fixture");
        file.write("[vprenderer]\nsdr_target_nits: 100\nsubtitle_bbox_test: true\n");
    }
    for (const char* white : { "120", "140" })
    {
        ConfigEditorWindow window(path, 0, true);
        requireControl<QLineEdit>(window, "config.vprenderer.sdr_target_nits")->setText(white);
        save(window);
        ConfigFile config;
        require(config.Load(path.toStdString()), "Saved subtitle config failed to reload");
        RendererProfileConfig::Model model;
        std::string error;
        require(RendererProfileConfig::Read(config, model, error), error.c_str());
        bool found = false;
        for (const auto& profile : model.profiles)
        {
            const auto setting = profile.second.settings.find("subtitle_bbox_test");
            if (setting != profile.second.settings.end())
            {
                require(setting->second == "true", "Config changed the subtitle diagnostic value");
                found = true;
            }
        }
        require(!found, "Legacy subtitle controls still affect renderer profiles");
    }
}

// Pruning 2026-10-17 (RetiredSettings.h): a file holding retired keys and values opens,
// saves and still loads in VP; Config keeps them as written (configuration is user data).
void testRetiredRendererSettingsSurviveConfigSaveAndReopen()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "Cannot create retired-settings fixture");
        file.write("[vprenderer]\nsdr_target_nits: 100\nfix_scene_cooldown: off\n"
            "[vprenderer.Cinema]\nquality: high\ndejudder_picture_area: off\ndither_vectors_new_frame: off\n"
            "adaptive_faithful: polar\ndejudder_even_rates: 30_and_29.97\n");
    }
    {
        ConfigEditorWindow window(path, 0, true);
        requireControl<QLineEdit>(window, "config.vprenderer.sdr_target_nits")->setText(QStringLiteral("120"));
        save(window);
    }
    ConfigFile config;
    require(config.Load(path.toStdString()), "Saved retired-settings config failed to reload");
    RendererProfileConfig::Model model;
    std::string error;
    require(RendererProfileConfig::Read(config, model, error), ("VP rejects retired settings after a save: " + error).c_str());
    const QByteArray saved = readBytes(path);
    for (const char* kept : { "fix_scene_cooldown: off", "dejudder_picture_area: off", "dither_vectors_new_frame: off",
            "adaptive_faithful: polar", "dejudder_even_rates: 30_and_29.97" })
        require(saved.contains(kept), (std::string("Config dropped or changed ") + kept).c_str());
}

void testSubtitleCutPasteSurvivesConfigSaveAndReopen()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "Cannot create subtitle diagnostic fixture");
        file.write("[vprenderer]\nsdr_target_nits: 100\nsubtitle_cut_paste_test: true\n"
            "subtitle_cut_paste_background: dark_gray\nsubtitle_box_padding_sides: 60\n"
            "subtitle_box_padding_top: 40\nsubtitle_box_padding_bottom: 12\nsubtitle_move_inset: 20\n");
    }
    for (const char* white : { "120", "140" })
    {
        ConfigEditorWindow window(path, 0, true);
        requireControl<QLineEdit>(window, "config.vprenderer.sdr_target_nits")->setText(white);
        save(window);
        ConfigFile config;
        require(config.Load(path.toStdString()), "Saved subtitle config failed to reload");
        RendererProfileConfig::Model model;
        std::string error;
        require(RendererProfileConfig::Read(config, model, error), error.c_str());
        bool found = false;
        for (const auto& profile : model.profiles)
        {
            const auto setting = profile.second.settings.find("subtitle_cut_paste_test");
            if (setting != profile.second.settings.end())
            {
                require(setting->second == "true", "Config changed the subtitle diagnostic value");
                found = true;
            }
        }
        require(!found, "Legacy subtitle controls still affect renderer profiles");
        for (const auto& expected : std::map<std::string,std::string>{
            {"subtitle_cut_paste_background","dark_gray"}, {"subtitle_box_padding_sides","60"},
            {"subtitle_box_padding_top","40"}, {"subtitle_box_padding_bottom","12"},
            {"subtitle_move_inset","20"}})
        {
            bool preserved=false;
            for (const auto& profile : model.profiles) {
                const auto entry=profile.second.settings.find(expected.first);
                if(entry!=profile.second.settings.end()) {
                    require(entry->second==expected.second,"Config changed subtitle mode or geometry");
                    preserved=true;
                }
            }
            require(!preserved,"Legacy subtitle geometry still affects renderer profiles");
            require(readBytes(path).contains(QByteArray::fromStdString(expected.first+": "+expected.second)),"Ignored legacy configuration was not preserved");
        }
    }
}

void testToneSliderDragPerformance()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create multi-profile drag fixture");
    file.write("[vprenderer.First]\nquality: high\n");
    for (int i=0;i<8;++i) file.write(QString("[vprenderer.Child%1]\nquality: high\n").arg(i).toUtf8());
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* slider = requireControl<QSlider>(window,"config.vprenderer.smoothing_period.slider");
    auto* edit = requireControl<QLineEdit>(window,"config.vprenderer.smoothing_period");
    int commits=0;
    QObject::connect(edit,&QLineEdit::textChanged,&window,[&] { ++commits; });
    slider->setSliderDown(true);
    QElapsedTimer elapsed;elapsed.start();
    for(int i=1;i<=200;++i) slider->setValue(i*4517);
    const qint64 dragMs=elapsed.elapsed();
    const int duringDrag=commits;
    const QString preview=edit->text();
    slider->setSliderDown(false);
    std::cout << "DRAG: 200 updates=" << dragMs << "ms; commits during=" << duringDrag << "; total=" << commits << std::endl;
    require(duringDrag==0 && commits==1,"Dragging must preview locally and commit exactly once on release");
    // Wall-clock budgets flake on a loaded machine (parallel shards, builds): the timing is reported,
    // and enforced only when VP_TEST_TIMING=1.
    if (qEnvironmentVariable("VP_TEST_TIMING") == QStringLiteral("1"))
        require(dragMs<500,"Slider drag processing exceeded 2.5 ms per update");
    else if (dragMs>=500)
        std::cout << "TIMING (report only, VP_TEST_TIMING=1 enforces): slider drag " << dragMs << " ms >= 500 ms" << std::endl;
    require(preview=="903" && edit->text()==preview,"Smoothing slider must use whole-frame increments and retain its preview");
    save(window);
    require(readBytes(path).contains("smoothing_period: 903"),"Release did not persist the final slider value");
    auto* low = requireControl<QLineEdit>(window,"config.vprenderer.scene_threshold_low");
    auto* high = requireControl<QLineEdit>(window,"config.vprenderer.scene_threshold_high");
    auto* lowSlider = requireControl<QSlider>(window,"config.vprenderer.scene_threshold_low.slider");
    auto* highSlider = requireControl<QSlider>(window,"config.vprenderer.scene_threshold_high.slider");
    lowSlider->setValue(1000000);
    require(low->text()=="2.99" && high->text()=="3", "Low slider crossed the high threshold");
    highSlider->setValue(0);
    require(high->text()=="0", "High slider lost the explicit Off detent");
    low->setText("100");
    highSlider->setValue(1000000);
    require(high->text()=="0", "High slider produced an invalid positive threshold above low=100");
    high->setText("3");
    lowSlider->setValue(0);
    lowSlider->setValue(1000000);
    require(low->text().toDouble()<3,"Slider did not repair an out-of-range typed threshold");
    high->setText("0.001");
    lowSlider->setValue(0);
    lowSlider->setValue(1000000);
    require(low->text().toDouble()<.001,"Fine typed high threshold produced an invalid low slider range");
}

void testToneMappingControls()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create tone fixture");
    file.write("[vprenderer.First]\nquality: high\nknee_adaptation: 0.6\n[vprenderer.Second]\nquality: balanced\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto editor = [&](const char* key) { return requireControl<QLineEdit>(window, QString("config.vprenderer.")+key); };
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    auto* slider = requireControl<QSlider>(window, "config.vprenderer.knee_adaptation.slider");
    auto* reset = requireControl<QToolButton>(window, "config.vprenderer.knee_adaptation.reset");
    require(editor("knee_adaptation")->text() == "0.6", "Saved tuning not loaded");
    require(editor("percentile")->text() == "99.995", "HQ percentile default incorrect");
    slider->setValue(750000);
    require(editor("knee_adaptation")->text() == "0.75", "Slider did not update numeric field");
    editor("knee_adaptation")->setText("0.125");
    require(slider->value() == 125000, "Numeric field did not update slider");
    profiles->setCurrentRow(1);
    require(editor("knee_adaptation")->text() == "0.125", "Named profile did not inherit tuning");
    require(editor("percentile")->text() == "100", "Balanced percentile default incorrect");
    reset->click();
    require(editor("knee_adaptation")->text() == "0.4" && reset->toolTip().contains("0.4"), "Reset did not show built-in value");
    save(window);
    const auto saved = readBytes(path);
    require(saved.contains("knee_adaptation: AUTO"), "Reset did not explicitly override inherited value");
    require(!saved.contains("percentile:"), "Merely viewing defaults changed configuration");
    editor("smoothing_period")->setText("1001");
    require(!apply->isEnabled() && editor("smoothing_period")->text() == "1001", "Invalid typed value was not retained and rejected");
    editor("smoothing_period")->setText("1000");
    editor("scene_threshold_low")->setText("5");
    require(!apply->isEnabled(), "Invalid inherited scene threshold pair accepted");
    editor("scene_threshold_high")->setText("6");
    require(apply->isEnabled(), "Valid scene threshold pair rejected");
    editor("knee_minimum")->setText("0.45");
    require(editor("knee_default")->text() == "0.45", "Automatic fallback knee did not follow bounds");
    editor("knee_default")->setText("0.4");
    require(!apply->isEnabled(), "Explicit fallback below knee minimum accepted");
    requireControl<QToolButton>(window,"config.vprenderer.knee_default.reset")->click();
    require(apply->isEnabled(), "Fallback reset did not clear validation");
    auto* curve = requireControl<QComboBox>(window,"config.vprenderer.tone_mapping");
    curve->setCurrentIndex(curve->findData("bt2390"));
    require(!editor("slope_tuning")->isEnabled() && editor("knee_offset")->isEnabled(), "Curve applicability incorrect");
    auto* peak = requireControl<QComboBox>(window,"config.vprenderer.peak_detection");
    peak->setCurrentIndex(peak->findData("off"));
    require(!editor("percentile")->isEnabled(), "Peak Off did not disable tuning controls");
    save(window);
    ConfigEditorWindow reloaded(path, 0, true);
    requireControl<QListWidget>(reloaded,"config.vprenderer.profiles")->setCurrentRow(1);
    require(requireControl<QLineEdit>(reloaded,"config.vprenderer.knee_adaptation")->text()=="0.4", "Reset did not survive reload");
    require(requireControl<QLineEdit>(reloaded,"config.vprenderer.scene_threshold_high")->text()=="6", "Peak settings did not survive reload");
    requireControl<QPushButton>(window,"config.vprenderer.tonePeak.restore_defaults")->click();
    require(editor("scene_threshold_low")->text()=="1" && editor("scene_threshold_high")->text()=="3" &&
        editor("smoothing_period")->text()=="20", "Section restore did not restore all defaults");
    const QString captures = qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
    if (!captures.isEmpty()) {
        profiles->setCurrentRow(0);
        window.selectPage(2);
        window.resize(1120, 1120);
        window.show();
        for (const char* id : { "toneSpline", "toneOther", "tonePeak" }) {
            auto* toggle = requireControl<QToolButton>(window, QString("rendererSection.")+id);
            toggle->setChecked(true);
            auto* section = requireControl<QWidget>(window, QString("rendererSection.")+id+".content")->parentWidget();
            QCoreApplication::processEvents();
            for (QWidget* parent=section->parentWidget(); parent; parent=parent->parentWidget())
                if (auto* scroll=qobject_cast<QScrollArea*>(parent)) { scroll->ensureWidgetVisible(section); break; }
            QCoreApplication::processEvents();
            require(window.grab().save(QDir(captures).filePath(QString(id)+".png")), "Cannot capture tone controls");
            toggle->setChecked(false);
        }
    }
}

void testBetaToneMappingControls()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create Beta fixture");
    file.write("[general]\nrenderer: VP Renderer\n[vprenderer.First]\nquality: high\n[vprenderer.Second]\nquality: balanced\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    window.setRendererDiscoveryForTesting({"VP Renderer", "VP Renderer (Beta)"}, {"VP Renderer", "VP Renderer (Beta)"});
    auto* renderer = requireControl<QComboBox>(window, "config.general.renderer");
    auto* dynamic = requireControl<QCheckBox>(window, "config.vprenderer.dynamic_target");
    auto* custom = requireControl<QCheckBox>(window, "config.vprenderer.custom_tone_mapping");
    require(!custom->isChecked() && !custom->isEnabled(), "Gamut overflow correction must default Off and be disabled in release");
    require(custom->accessibleName() == "Gamut overflow correction (Experimental)", "Correction label must identify experimental status");
    require(custom->toolTip().contains("near or beyond") && custom->toolTip().contains("after the selected gamut mapping") && custom->toolTip().contains("May reduce brightness or saturation") && custom->toolTip().contains("Off: original"), "Correction tooltip must explain placement, tradeoff and original path");
    auto* gamut = requireControl<QComboBox>(window, "config.vprenderer.gamut_mapping");
    auto* film = requireControl<QLineEdit>(window, "config.vprenderer.film_strength");
    auto* clip = requireControl<QLineEdit>(window, "config.vprenderer.mastering_clip");
    require(dynamic->isChecked() && film->text() == "0.3" && clip->text() == "1", "Beta defaults incorrect");
    require(!dynamic->isEnabled() && !film->isEnabled() && !clip->isEnabled(), "Stable must disable Beta controls");
    renderer->setCurrentIndex(renderer->findData("VP Renderer (Beta)"));
    require(custom->isEnabled() && dynamic->isEnabled() && film->isEnabled() && clip->isEnabled(), "Beta controls did not enable");
    custom->setChecked(true);
    require(gamut->isEnabled(), "Upstream gamut choice must remain available with residual correction");
    gamut->setCurrentIndex(gamut->findData("relative"));
    dynamic->setChecked(false); film->setText("0.75"); clip->setText("0.5");
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(1);
    require(custom->isChecked() && !dynamic->isChecked() && film->text() == "0.75" && gamut->property("effectiveValue").toString() == "relative", "Beta settings and selected gamut did not inherit");
    renderer->setCurrentIndex(renderer->findData("VP Renderer"));
    require(!custom->isEnabled() && custom->isChecked() && !film->isEnabled() && film->text() == "0.75", "Stable selection changed saved Beta values");
    require(requireControl<QComboBox>(window, "config.vprenderer.gamut_mapping")->isEnabled(), "Stable shared gamut control must remain available");
    save(window);
    const auto saved = readBytes(path);
    require(saved.contains("gamut_mapping: relative"), "Upstream gamut selection was not saved with correction enabled");
    require(saved.contains("custom_tone_mapping: true") && saved.contains("dynamic_target: false") && saved.contains("film_strength: 0.75") && saved.contains("mastering_clip: 0.5"), "Beta fields were not saved");
    ConfigEditorWindow enabledReload(path, 0, true);
    enabledReload.setRendererDiscoveryForTesting({"VP Renderer", "VP Renderer (Beta)"}, {"VP Renderer", "VP Renderer (Beta)"});
    requireControl<QListWidget>(enabledReload, "config.vprenderer.profiles")->setCurrentRow(1);
    require(requireControl<QCheckBox>(enabledReload, "config.vprenderer.custom_tone_mapping")->isChecked(), "Existing persisted custom_tone_mapping key must reload correction On");
    requireControl<QComboBox>(enabledReload, "config.general.renderer")->setCurrentIndex(
        requireControl<QComboBox>(enabledReload, "config.general.renderer")->findData("VP Renderer (Beta)"));
    require(requireControl<QComboBox>(enabledReload, "config.vprenderer.gamut_mapping")->isEnabled(), "Reloaded correction must preserve upstream gamut editing");
    renderer->setCurrentIndex(renderer->findData("VP Renderer (Beta)"));
    requireControl<QPushButton>(window, "config.vprenderer.toneHighlightBeta.restore_defaults")->click();
    require(!custom->isChecked() && dynamic->isChecked() && film->text() == "0.3" && clip->text() == "1", "Beta restore failed");
    require(gamut->isEnabled() && gamut->property("effectiveValue").toString() == "relative", "Correction reset must retain upstream gamut choice");
    film->setText("2.1");
    require(!requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(), "Invalid Beta field accepted");
    film->setText("0.3"); save(window);
    ConfigEditorWindow reloaded(path, 0, true);
    requireControl<QListWidget>(reloaded, "config.vprenderer.profiles")->setCurrentRow(1);
    require(requireControl<QCheckBox>(reloaded, "config.vprenderer.dynamic_target")->isChecked(), "Dynamic target reset did not survive reload");
    require(!requireControl<QCheckBox>(reloaded, "config.vprenderer.custom_tone_mapping")->isChecked(), "Original mapping reset did not survive reload");
    const QString captures = qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
    if (!captures.isEmpty()) {
        window.selectPage(2); window.resize(1440, 1100); window.show();
        requireControl<QToolButton>(window, "rendererSection.toneHighlightBeta")->setChecked(true);
        QCoreApplication::processEvents();
        require(window.grab().save(QDir(captures).filePath("beta-highlight.png")), "Cannot capture Beta controls");
        renderer->setCurrentIndex(renderer->findData("VP Renderer"));
        QCoreApplication::processEvents();
        require(window.grab().save(QDir(captures).filePath("stable-highlight.png")), "Cannot capture stable controls");
    }
}

void testHdrTargetLuminanceValidationRetainsSavedValue()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    ConfigEditorWindow window(path, 0, true);
    auto* white = requireControl<QLineEdit>(window, "config.vprenderer.sdr_target_nits");
    auto* black = requireControl<QLineEdit>(window, "config.vprenderer.sdr_black_nits");
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    auto* status = requireControl<QLabel>(window, "configurationStatus");
    black->setText("0");
    for (const QString& valid : { "0.000002", "0.01", "1", "39", "501", "600", "4000", "10000" })
    {
        white->setText(valid);
        require(white->hasAcceptableInput(), "Luminance widget still restricts libplacebo's range");
        save(window);
        require(readBytes(path).contains(("sdr_target_nits: " + valid).toUtf8()), "Expanded white did not persist exactly");
    }
    const auto saved = readBytes(path);
    for (const QString& invalid : { "10000.1", "0", "-1", "0.000001", "1e-12", "nan", "inf" })
    {
        white->setText(invalid);
        QCoreApplication::processEvents();
        require(!apply->isEnabled() && status->text().contains("10000"), "Invalid white not visibly rejected");
        require(white->text() == invalid && readBytes(path) == saved, "Invalid white changed the saved config");
    }
    white->setText("10000");
    for (const QString& invalid : { "-1", "10000", "10001", "nan", "inf" })
    {
        black->setText(invalid);
        QCoreApplication::processEvents();
        require(!apply->isEnabled() && status->text().contains("sdr_black_nits"), "Invalid black not rejected");
    }
    black->setText("600");
    require(black->hasAcceptableInput(), "Black still has a 500-nit cap");
    save(window);
    ConfigEditorWindow reloaded(path, 0, true);
    require(requireControl<QLineEdit>(reloaded, "config.vprenderer.sdr_target_nits")->text() == "10000" &&
        requireControl<QLineEdit>(reloaded, "config.vprenderer.sdr_black_nits")->text() == "600", "Expanded luminance failed reload");
}

void testBlackDefaultAndLegacyAutoInheritance()
{
    for (const QByteArray blackLine : { QByteArray(), QByteArray("sdr_black_nits: AUTO\n"),
        QByteArray("sdr_black_nits: 0.025\n") })
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("VideoProcessor.cfg");
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "Cannot create black default fixture");
        file.write("[vprenderer.First]\nsdr_target_nits: 100\n" + blackLine +
            "[vprenderer.Second]\nsdr_target_nits: 200\n");
        file.close();
        ConfigEditorWindow window(path, 0, true);
        window.setActiveProfileStatusForTesting({}, {}, {}, {}, {});
        auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
        auto* black = requireControl<QLineEdit>(window, "config.vprenderer.sdr_black_nits");
        auto* status = requireControl<QLabel>(window, "config.vprenderer.sdr_black_nits.auto_status");
        for (int row : { 0, 1, 0 })
        {
            profiles->setCurrentRow(row);
            QCoreApplication::processEvents();
            const QString expected = blackLine.isEmpty() ? "0" :
                blackLine.contains("AUTO") ? "AUTO" : "0.025";
            require(black->text().compare(expected, Qt::CaseInsensitive) == 0,
                "Default/inherited black differs from effective value");
            if (blackLine.contains("AUTO"))
                require(status->text() == "Auto: 0 nits (0.000001 nit internally)",
                    "Legacy Auto black still depends on inherited white");
        }
        auto* white = requireControl<QLineEdit>(window, "config.vprenderer.sdr_target_nits");
        white->setText("150");
        save(window);
        ConfigEditorWindow reloaded(path, 0, true);
        requireControl<QListWidget>(reloaded, "config.vprenderer.profiles")->setCurrentRow(1);
        require(requireControl<QLineEdit>(reloaded, "config.vprenderer.sdr_black_nits")->text()
            .compare(black->text(), Qt::CaseInsensitive) == 0,
            "Black default/inherited value changed on save and reload");
    }
}

void testInheritedHdrTargetLuminanceValidation()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create inherited luminance fixture");
    file.write("[vprenderer.First]\nsdr_target_nits: 1000\nsdr_black_nits: 300\n"
        "[vprenderer.Second]\nsdr_target_nits: 2000\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(1);
    auto* white = requireControl<QLineEdit>(window, "config.vprenderer.sdr_target_nits");
    auto* black = requireControl<QLineEdit>(window, "config.vprenderer.sdr_black_nits");
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    white->setText("200");
    require(!apply->isEnabled(), "Inherited black above explicit white escaped validation");
    white->clear();
    black->setText("500");
    require(apply->isEnabled(), "Validation did not use inherited white of 1000");
    save(window);
}

void testAuditedBooleanAliasesAndInheritedAutoStatus()
{
    for (const QByteArray alias : { "true", "yes", "on", "1", "false", "no", "off", "0" })
    {
        QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
        QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
        file.write("[vprenderer.First]\nquality: fast\n[vprenderer.Child]\ntone_mapping: reinhard\n"
            "[vprenderer.color.First]\noutput_range: limited\noutput_transport_gamma: auto\noutput_diagnostics: " + alias +
            "\nreport_bt2020_to_display: " + alias + "\n[vprenderer.color.Child]\noutput_gamma: 2.2\n"
            "[vprenderer.zoom.First]\nsubtitle_fit: " + alias + "\n"); file.close();
        ConfigEditorWindow window(path, 0, true);
        window.setActiveProfileStatusForTesting({}, {}, {}, {}, {});
        requireControl<QListWidget>(window,"config.vprenderer.profiles")->setCurrentRow(1);
        requireControl<QListWidget>(window,"config.vprenderer.color.profiles")->setCurrentRow(1);
        const bool expected = alias == "true" || alias == "yes" || alias == "on" || alias == "1";
        for (const char* key : { "output_diagnostics", "report_bt2020_to_display" })
            require(requireControl<QCheckBox>(window,"config.vprenderer.color."+QString::fromLatin1(key))->isChecked() == expected,"Boolean alias differs from runtime");
        require(requireControl<QLabel>(window,"config.vprenderer.peak_detection.auto_status")->text() == "Auto: Off", "Inherited Fast shown as High");
        require(requireControl<QLabel>(window,"config.vprenderer.color.output_transport_gamma.auto_status")->text() == "Auto: 2.4", "Inherited Limited shown unused");
        requireControl<QListWidget>(window,"config.vprenderer.profiles")->setCurrentRow(0);
        selectData(requireControl<QComboBox>(window,"config.vprenderer.quality"),"balanced");
        requireControl<QListWidget>(window,"config.vprenderer.profiles")->setCurrentRow(1);
        require(requireControl<QLabel>(window,"config.vprenderer.peak_detection.auto_status")->text() == "Auto: Standard", "Inherited Balanced shown as High");
        save(window);
        require(readBytes(path).contains("output_diagnostics: " + alias), "Untouched Boolean alias rewritten");
    }
}

void testInvalidAuditedValuesRemainActionable()
{
    for (const QByteArray extra : {
        QByteArray("[vprenderer.scaling.First]\ndownscaler: typo_filter\n"),
        QByteArray("[vprenderer.zoom.First]\nfixed_crop_aspect: typo_aspect\n"),
        QByteArray("[vprenderer]\nquality: high\n[vprenderer.Base]\nquality: fast\n") })
    {
        QTemporaryDir dir; const QString path=dir.filePath("VideoProcessor.cfg");
        QFile file(path); require(file.open(QIODevice::WriteOnly),"fixture");
        file.write(extra); file.close();
        ConfigEditorWindow window(path,0,true);
        requireControl<QLineEdit>(window,"config.vprenderer.sdr_target_nits")->setText("210");
        // An edit that always lands (the file may have no renderer profile to edit).
        requireControl<QSpinBox>(window,"config.general.profile_change_display_seconds")->setValue(7);
        require(readBytes(path)==extra,"Opening invalid input changed the file");
        requireControl<QPushButton>(window,"applyConfiguration")->click();
        require(readBytes(path)==extra,(QByteArray("Apply silently saved invalid input: ")+extra+" saved: "+readBytes(path)).constData());
        const QString status = requireControl<QLabel>(window,"configurationStatus")->text();
        require(status.contains(extra.contains("typo_filter") ? "downscaler" :
            extra.contains("typo_aspect") ? "fixed_crop_aspect" : "conflicts"),
            ("Validation error was not actionable: " + status).toStdString().c_str());
        if (extra.contains("typo_filter"))
            require(requireControl<QComboBox>(window,"config.vprenderer.scaling.downscaler")->currentData()=="typo_filter","Unknown downscaler was erased");
        if (extra.contains("typo_aspect"))
            require(requireControl<QLineEdit>(window,"config.vprenderer.zoom.fixed_crop_aspect")->text()=="typo_aspect","Invalid fixed crop erased");
    }
}

void testDiagnosticPresetDescribesFlagsAndRetentionUsesDefault()
{
    QTemporaryDir dir; const QString path=dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly),"fixture");
    file.write("[vprenderer.First]\nquality: high\n[vprenderer.color.First]\noutput_path_profile: proposed\n"
        "[vprenderer.color.Child]\noutput_diagnostics: yes\n[logging]\ndebug_log_retention: 500\n"); file.close();
    ConfigEditorWindow window(path,0,true);
    auto* preset=requireControl<QComboBox>(window,"config.vprenderer.color.output_path_profile");
    require(preset->currentData()=="legacy","Handwritten preset falsely reports enabled diagnostics");
    require(!requireControl<QCheckBox>(window,"config.vprenderer.color.output_diagnostics")->isChecked(),"Loading preset enabled diagnostics");
    requireControl<QListWidget>(window,"config.vprenderer.color.profiles")->setCurrentRow(1);
    require(preset->currentData()=="proposed","Preset ignores effective diagnostic flags");
    require(requireControl<QSpinBox>(window,"config.logging.debug_log_retention")->value()==10,"Invalid retention does not show runtime default");
    requireControl<QCheckBox>(window,"config.vprenderer.color.diagnostic_disable_compute")->setChecked(true);
    require(preset->currentData()=="custom","Edited flag did not derive Custom");
    requireControl<QCheckBox>(window,"config.vprenderer.color.diagnostic_disable_compute")->setChecked(false);
    require(preset->currentData()=="proposed","Returning to preset flags did not derive preset");
}

void testOmittedAndAutoFrameOffsetDisplayNinety()
{
    for (const QByteArray line : { QByteArray(), QByteArray("frame_offset: AUTO\n"), QByteArray("frame_offset: 0\n") })
    {
        QTemporaryDir dir; const QString path=dir.filePath("VideoProcessor.cfg");
        QFile file(path); require(file.open(QIODevice::WriteOnly),"fixture");
        file.write("[directshow]\n"+line+"[vprenderer.First]\nquality: high\n"
            "[vprenderer.viewport.First]\nautomatic_crop: false\n"); file.close();
        ConfigEditorWindow window(path,0,true);
        auto* aspect = requireControl<QLineEdit>(window,"config.vprenderer.viewport.screen_aspect");
        require(aspect->text().isEmpty() && aspect->placeholderText().contains("output display"),
            "Omitted screen aspect fabricates a 16:9 setting");
        const bool automatic=!line.contains(": 0");
        require(requireControl<QCheckBox>(window,"config.directshow.frame_offset.auto")->isChecked()==automatic,"Wrong timing mode");
        require(requireControl<QSpinBox>(window,"config.directshow.frame_offset.value")->value()==(automatic?90:0),"Wrong timing default");
    }
}

void testDejudderBoxPresetsAndMask()
{
    // Owner 2026-10-16 (docs/VP-motion-slider.md): the Motion slider. Each position writes exactly the
    // table's values; an edit shows Custom; Reset to preset restores; unset = Not set; a saved value
    // that is no position = Custom.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.Cinema]\nquality: high\n[vprenderer.Other]\ndejudder_strength: 0.3\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* slider = requireControl<QSlider>(window, "config.vprenderer.motion_preset");
    auto* name = requireControl<QLabel>(window, "config.vprenderer.motion_preset_name");
    auto* reset = requireControl<QPushButton>(window, "config.vprenderer.motion_preset_reset");
    auto* strength = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_strength");
    auto* even = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_strength_even");
    auto* object = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_object_strength");
    auto* floor = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_roll_floor");
    auto* rollLo = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_roll_lo");
    auto* rollHi = requireControl<QLineEdit>(window, "config.vprenderer.dejudder_roll_hi");
    auto* evenRates = requireControl<QComboBox>(window, "config.vprenderer.dejudder_even_rates");
    auto* rolloff = requireControl<QComboBox>(window, "config.vprenderer.dejudder_motion_rolloff");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(name->text() == "Custom", (QByteArray("A saved 0.3 is not shown as Custom: ") + name->text().toUtf8()).constData());
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    require(name->text().startsWith("Not set"), (QByteArray("No motion keys is not shown as Not set: ") + name->text().toUtf8()).constData());
    struct Row { int position; const char* strength; const char* even; const char* rates; const char* object; const char* rolloff;
        const char* floor; const char* lo; const char* hi; const char* label; };
    const Row rows[] = {
        { 1, "0.15", "0", "whole", "0.4", "on", "0.15", "6", "18", "Film" },
        { 2, "0.25", "0", "whole", "0.5", "on", "0.25", "8", "24", "Cinema" },
        { 3, "0.4", "0.5", "whole_and_1001", "0.7", "on", "0.5", "12", "36", "Natural" },
        { 4, "0.65", "0.8", "whole_and_1001", "0.85", "on", "0.75", "16", "48", "Smooth" },
        { 5, "1", "1", "whole_and_1001", "1", "off", "1", "16", "48", "Max" },
        { 0, "0", "0", "whole", "1", "off", "0.25", "8", "24", "As shot" } };
    for (const Row& r : rows)
    {
        slider->setValue(r.position);
        QCoreApplication::processEvents();
        const QByteArray at = QByteArray("position ") + r.label + ": ";
        require(strength->text() == r.strength, (at + "strength " + strength->text().toUtf8()).constData());
        require(even->text() == r.even, (at + "even strength " + even->text().toUtf8()).constData());
        require(evenRates->currentData().toString() == r.rates, (at + "even rates").constData());
        require(object->text() == r.object, (at + "object strength " + object->text().toUtf8()).constData());
        require(rolloff->currentData().toString() == r.rolloff, (at + "roll-off").constData());
        require(floor->text() == r.floor && rollLo->text() == r.lo && rollHi->text() == r.hi, (at + "roll-off band").constData());
        require(name->text().startsWith(r.label), (at + "label " + name->text().toUtf8()).constData());
    }
    // Max, then an edit: Custom without moving the slider; reset restores.
    slider->setValue(5);
    strength->setText("0.9");
    QCoreApplication::processEvents();
    require(name->text() == "Custom" && slider->value() == 5, "Editing a moved key did not show Custom (slider kept)");
    reset->click();
    QCoreApplication::processEvents();
    require(strength->text() == "1" && name->text().startsWith("Max"), "Reset to preset did not restore Max");
    // Freehand boxes: out-of-range or over-precise values are clamped and rounded when the field is left;
    // each field's own reset puts it back to the slider position's value.
    slider->setValue(5);
    object->setText("0.777");
    QMetaObject::invokeMethod(object, "editingFinished");
    QCoreApplication::processEvents();
    require(object->text() == "0.78", (QByteArray("0.777 was not rounded to the field's 2 decimals: ") + object->text().toUtf8()).constData());
    rollHi->setText("999");
    QMetaObject::invokeMethod(rollHi, "editingFinished");
    QCoreApplication::processEvents();
    require(rollHi->text() == "400", (QByteArray("999 px was not clamped to 400: ") + rollHi->text().toUtf8()).constData());
    QAction* objectReset = nullptr;
    for (QAction* action : object->actions())
        if (action->objectName().endsWith(".reset")) objectReset = action;
    require(objectReset != nullptr, "The field has no reset");
    objectReset->trigger();
    QCoreApplication::processEvents();
    require(object->text() == "1", (QByteArray("The field reset did not restore Max's 1: ") + object->text().toUtf8()).constData());
    reset->click();
    QCoreApplication::processEvents();
    require(name->text().startsWith("Max"), "Reset to preset after the edits did not restore Max");
    require(object->accessibleDescription().contains("0-1"), "No allowed range noted for the field");
    // Default: one click back to Cinema and its values, from anywhere (here an edited Max).
    strength->setText("0.9");
    requireControl<QPushButton>(window, "config.vprenderer.motion_preset_default")->click();
    QCoreApplication::processEvents();
    require(slider->value() == 2 && strength->text() == "0.25" && object->text() == "0.5" && name->text().startsWith("Cinema"),
        "Default did not restore Cinema");
    // Natural saved, with the mask and the extended range.
    slider->setValue(3);
    selectData(requireControl<QComboBox>(window, "config.vprenderer.occlusion_mask"), "on");
    selectData(requireControl<QComboBox>(window, "config.vprenderer.preserve_extended_range"), "on");
    QCoreApplication::processEvents();
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    require(apply->isEnabled(), "Motion edits did not enable Apply");
    apply->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    for (const char* expected : { "dejudder_strength: 0.4", "dejudder_strength_even: 0.5", "dejudder_even_rates: whole_and_1001",
        "dejudder_object_strength: 0.7", "dejudder_motion_rolloff: on", "dejudder_roll_floor: 0.5", "occlusion_mask: on",
        "preserve_extended_range: on", "dejudder_strength: 0.3" })
        require(saved.contains(expected), (QByteArray("Motion setting not saved: ") + expected).constData());
    // Reopened: the slider reads the saved fields back as Natural.
    ConfigEditorWindow reopened(path, 0, true);
    auto* profiles2 = requireControl<QListWidget>(reopened, "config.vprenderer.profiles");
    profiles2->setCurrentRow(0);
    QCoreApplication::processEvents();
    require(requireControl<QSlider>(reopened, "config.vprenderer.motion_preset")->value() == 3 &&
        requireControl<QLabel>(reopened, "config.vprenderer.motion_preset_name")->text().startsWith("Natural"),
        "A reopened Config does not show the saved Natural");
}

void testPictureSliderPresets()
{
    // Owner 2026-10-16: the Picture slider (Off, Cautious, Standard, Fast) moves source_resolution and
    // auto's evidence; an edit shows Custom; Reset to preset / Default / the field resets restore; the
    // freehand boxes clamp to their ranges; saved and read back.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.Cinema]\nquality: high\n[vprenderer.Other]\nsource_resolution: 1080\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* slider = requireControl<QSlider>(window, "config.vprenderer.picture_preset");
    auto* name = requireControl<QLabel>(window, "config.vprenderer.picture_preset_name");
    auto* reset = requireControl<QPushButton>(window, "config.vprenderer.picture_preset_reset");
    auto* mode = requireControl<QComboBox>(window, "config.vprenderer.source_resolution");
    auto* clarity = requireControl<QLineEdit>(window, "config.vprenderer.source_resolution_auto_min_confidence");
    auto* seconds = requireControl<QLineEdit>(window, "config.vprenderer.source_resolution_evidence_seconds");
    auto* shots = requireControl<QLineEdit>(window, "config.vprenderer.source_resolution_evidence_shots");
    // Organisation pass 2: the Scaling tier owns re-base on/off (source_resolution native / auto), so the
    // slider has no Off of its own; its positions set how much evidence re-base needs.
    require(slider->minimum() == 1, "the picture slider offers its own Off (the Scaling tier owns it)");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(name->text() == "Custom", (QByteArray("A fixed 1080 is not shown as Custom: ") + name->text().toUtf8()).constData());
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    require(name->text().startsWith("Standard (default)") && slider->value() == 2,
        (QByteArray("The default profile with no keys is not shown as the default Standard: ") + name->text().toUtf8()).constData());
    struct Row { int position; const char* mode; const char* clarity; const char* seconds; const char* shots; const char* label; };
    const Row rows[] = {
        { 1, "auto", "0.9", "20", "4", "Cautious" },
        { 2, "auto", "0.8", "10", "3", "Standard" },
        { 3, "auto", "0.7", "5", "2", "Fast" } };
    for (const Row& r : rows)
    {
        slider->setValue(r.position);
        QCoreApplication::processEvents();
        const QByteArray at = QByteArray("position ") + r.label + ": ";
        require(mode->currentData().toString() == r.mode, (at + "source_resolution " + mode->currentData().toString().toUtf8()).constData());
        require(clarity->text() == r.clarity && seconds->text() == r.seconds && shots->text() == r.shots,
            (at + "evidence " + clarity->text().toUtf8() + " " + seconds->text().toUtf8() + " " + shots->text().toUtf8()).constData());
        require(name->text().startsWith(r.label), (at + "label " + name->text().toUtf8()).constData());
    }
    // Standard, then an edit: Custom without moving the slider; reset restores.
    slider->setValue(2);
    shots->setText("5");
    QCoreApplication::processEvents();
    require(name->text() == "Custom" && slider->value() == 2, "Editing a key did not show Custom (slider kept)");
    reset->click();
    QCoreApplication::processEvents();
    require(shots->text() == "3" && name->text().startsWith("Standard"), "Reset to preset did not restore Standard");
    // Freehand boxes: clamped to the range and rounded (whole shots) when the field is left.
    seconds->setText("500");
    QMetaObject::invokeMethod(seconds, "editingFinished");
    shots->setText("2.6");
    QMetaObject::invokeMethod(shots, "editingFinished");
    QCoreApplication::processEvents();
    require(seconds->text() == "120", (QByteArray("500 s was not clamped to 120: ") + seconds->text().toUtf8()).constData());
    require(shots->text() == "3", (QByteArray("2.6 shots was not rounded to a whole 3: ") + shots->text().toUtf8()).constData());
    QAction* secondsReset = nullptr;
    for (QAction* action : seconds->actions())
        if (action->objectName().endsWith(".reset")) secondsReset = action;
    require(secondsReset != nullptr, "The field has no reset");
    secondsReset->trigger();
    QCoreApplication::processEvents();
    require(seconds->text() == "10", (QByteArray("The field reset did not restore Standard's 10: ") + seconds->text().toUtf8()).constData());
    require(seconds->accessibleDescription().contains("2-120"), "No allowed range noted for the field");
    // Default: back to Standard (auto) from anywhere (here an edited Standard moved to Fast).
    slider->setValue(3);
    requireControl<QPushButton>(window, "config.vprenderer.picture_preset_default")->click();
    QCoreApplication::processEvents();
    require(slider->value() == 2 && mode->currentData().toString() == "auto" && shots->text() == "3" && name->text().startsWith("Standard"),
        "Default did not restore Standard");
    // Fast saved and read back.
    slider->setValue(3);
    QCoreApplication::processEvents();
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    require(apply->isEnabled(), "Picture edits did not enable Apply");
    apply->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    // source_resolution stays unset (auto is the shipped value; the Scaling tier owns it).
    for (const char* expected : { "source_resolution_auto_min_confidence: 0.7",
        "source_resolution_evidence_seconds: 5", "source_resolution_evidence_shots: 2", "source_resolution: 1080" })
        require(saved.contains(expected), (QByteArray("Picture setting not saved: ") + expected).constData());
    ConfigEditorWindow reopened(path, 0, true);
    requireControl<QListWidget>(reopened, "config.vprenderer.profiles")->setCurrentRow(0);
    QCoreApplication::processEvents();
    require(requireControl<QSlider>(reopened, "config.vprenderer.picture_preset")->value() == 3 &&
        requireControl<QLabel>(reopened, "config.vprenderer.picture_preset_name")->text().startsWith("Fast"),
        "A reopened Config does not show the saved Fast");
}

void testShaderAddonsList()
{
    // Owner 2026-10-16: selecting a shader VP adds to lists VP's add-ons for it (cost, motion, recommended).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[shader.standard]\ntype: multi\nlabel: Off\n"
        "[shader.standard.ssim]\nshader_type: custom\nlabel: SSimDownscaler\nstage: post_resize\norder: 20\nglsl_file: SSimDownscaler.glsl\n"
        "[shader.standard.invert]\nshader_type: custom\nlabel: Invert\nstage: post_resize\norder: 30\nglsl_file: Invert.glsl\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* items = requireControl<QListWidget>(window, "config.shader.standard.items");
    auto* addons = requireControl<QLabel>(window, "config.shader.standard.addons");
    auto rowOf = [items](const char* text) {
        for (int i = 0; i < items->count(); ++i)
            if (items->item(i)->text().contains(text)) return i;
        return -1;
    };
    require(rowOf("SSimDownscaler") >= 0 && rowOf("Invert") >= 0, "shader items listed");
    items->setCurrentRow(rowOf("SSimDownscaler"));
    QCoreApplication::processEvents();
    require(!addons->isHidden() && addons->text().contains("SSIM / faithful blend") && addons->text().contains("Grain map from motion"),
        (QByteArray("SSimDownscaler does not list its add-ons: ") + addons->text().left(200).toUtf8()).constData());
    items->setCurrentRow(rowOf("Invert"));
    QCoreApplication::processEvents();
    require(addons->isHidden(), "a shader without VP add-ons shows the list");
}

void testExclusiveButtonsForLedSourceAndMotionSource()
{
    // Owner 2026-10-16: the LED command source and the motion source as mutually exclusive buttons,
    // bound to their keys (the drop-downs kept as hidden value holders).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.Cinema]\nquality: high\n[led_drive.Default]\nenabled: true\ncommand_source: internal_script\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* script = window.findChild<QRadioButton*>(QStringLiteral("config.led_drive.default.command_source.internal_script"));
    auto* compensate = window.findChild<QRadioButton*>(QStringLiteral("config.led_drive.default.command_source.compensate"));
    require(script && compensate, "the LED source buttons exist");
    require(script->isChecked() && !compensate->isChecked(), "the LED source buttons do not show the saved source");
    compensate->click();
    QCoreApplication::processEvents();
    require(!script->isChecked() && requireControl<QComboBox>(window, "config.led_drive.default.command_source")->currentData().toString() == "compensate",
        "the LED source buttons are not exclusive or not bound");
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
    QCoreApplication::processEvents();
    auto* engine = requireControl<QRadioButton>(window, "config.vprenderer.optical_flow.engine");
    auto* matcher = requireControl<QRadioButton>(window, "config.vprenderer.optical_flow.matcher");
    auto* flow = requireControl<QComboBox>(window, "config.vprenderer.optical_flow");
    require(engine->isChecked() && !matcher->isChecked(), "the engine (default) is not shown selected");
    // Organisation pass 2: optical_flow is a Motion tier key (VP-V > Motion > Advanced > Set by the Motion
    // tier), greyed until Custom.
    require(!matcher->isEnabled(), "the motion source is not set by the Motion tier");
    requireControl<QCheckBox>(window, "vpv.motion.card.tier_keys.custom")->setChecked(true);
    QCoreApplication::processEvents();
    matcher->click();
    QCoreApplication::processEvents();
    require(flow->currentData().toString() == "off" && !engine->isChecked(), "VP's matcher only did not set optical_flow off");
    require(!requireControl<QComboBox>(window, "config.vprenderer.optical_flow_global")->isEnabled(), "the engine's options are not greyed with the matcher only");
    engine->click();
    QCoreApplication::processEvents();
    require(flow->currentData().toString() == "on", "the engine button did not set optical_flow on");
}

void testTemporalDitherButtons()
{
    // Owner 2026-10-16: the dither as one exclusive choice (Off / libplacebo / VP temporal), saved as
    // temporal_dither; not set = as before (a note, no button).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.Cinema]\nquality: high\n[vprenderer.Other]\ntemporal_dither: libplacebo\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* off = requireControl<QRadioButton>(window, "config.vprenderer.temporal_dither.off");
    auto* libplacebo = requireControl<QRadioButton>(window, "config.vprenderer.temporal_dither.libplacebo");
    auto* sigma = requireControl<QRadioButton>(window, "config.vprenderer.temporal_dither.sigma_delta");
    auto* value = requireControl<QComboBox>(window, "config.vprenderer.temporal_dither");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(libplacebo->isChecked() && !off->isChecked() && !sigma->isChecked(), "the saved libplacebo is not shown");
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    // Audit 2026-10-17: unset shows Auto (VP's default), the one "Not set".
    require(!off->isChecked() && !libplacebo->isChecked() && !sigma->isChecked() &&
        requireControl<QRadioButton>(window, "config.vprenderer.temporal_dither.auto")->isChecked(),
        "an unset profile does not show Auto");
    // Organisation pass 2: temporal_dither is the Output tier's key, greyed until Custom.
    requireControl<QCheckBox>(window, "vpv.output.card.tier_keys.custom")->setChecked(true);
    QCoreApplication::processEvents();
    sigma->click();
    QCoreApplication::processEvents();
    require(value->currentData().toString() == "sigma_delta" && !libplacebo->isChecked(), "VP temporal did not set sigma_delta");
    off->click();
    QCoreApplication::processEvents();
    require(value->currentData().toString() == "off" && !sigma->isChecked(), "the buttons are not exclusive");
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("temporal_dither: off"), "temporal_dither not saved");
}

void testInterpolationButtons()
{
    // docs/VP-interpolator-50-60-plan.md: the 50/60p interpolation as exclusive buttons beside the
    // motion source (Off / Auto / Always; Hold / Blend), off and hold by default, the fallback and
    // the classification greyed while it is off; saved as interpolation / interpolation_fallback.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.Cinema]\nquality: high\n[vprenderer.Sports]\ninterpolation: auto\ninterpolation_fallback: blend\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* off = requireControl<QRadioButton>(window, "config.vprenderer.interpolation.off");
    auto* autoMode = requireControl<QRadioButton>(window, "config.vprenderer.interpolation.auto");
    auto* always = requireControl<QRadioButton>(window, "config.vprenderer.interpolation.always");
    auto* hold = requireControl<QRadioButton>(window, "config.vprenderer.interpolation_fallback.hold");
    auto* blend = requireControl<QRadioButton>(window, "config.vprenderer.interpolation_fallback.blend");
    auto* mode = requireControl<QComboBox>(window, "config.vprenderer.interpolation");
    auto* fallback = requireControl<QComboBox>(window, "config.vprenderer.interpolation_fallback");
    auto* sourceClass = requireControl<QComboBox>(window, "config.vprenderer.source_class");
    auto* fillGaps = requireControl<QComboBox>(window, "config.vprenderer.interpolation_fill_gaps");
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    require(off->isChecked() && !autoMode->isChecked() && !always->isChecked(), "off (the default) is not shown");
    require(hold->isChecked() && !blend->isChecked(), "hold (the default) is not shown");
    require(!sourceClass->isEnabled() && !fallback->isEnabled(), "the options are not greyed with interpolation off");
    // Owner 2026-10-17: hiccup repair is separate from motion smoothing - never greyed by it.
    // VP-V (ADR-0023): a control the VP-V page manages is greyed "Managed by VP-V", not by interpolation.
    require((fillGaps->isEnabled() || fillGaps->property("vpvManaged").toBool()) && fillGaps->property("greyedOff").toBool() == false,
        "hiccup repair is greyed with interpolation off");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(autoMode->isChecked() && blend->isChecked(), "the saved auto / blend are not shown");
    require(sourceClass->isEnabled() && (fillGaps->isEnabled() || fillGaps->property("vpvManaged").toBool()), "the options are greyed with interpolation on");
    always->click();
    QCoreApplication::processEvents();
    require(mode->currentData().toString() == "always" && !autoMode->isChecked(), "Always did not set interpolation always");
    hold->click();
    QCoreApplication::processEvents();
    require(fallback->currentData().toString() == "hold" && !blend->isChecked(), "the fallback buttons are not exclusive");
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    require(saved.contains("interpolation: always") && saved.contains("interpolation_fallback: hold"),
        (QByteArray("interpolation not saved: ") + saved.left(400)).constData());
    off->click();
    QCoreApplication::processEvents();
    require(mode->currentData().toString() == "off" && !sourceClass->isEnabled(), "Off did not set interpolation off");
}

void testContentAdaptiveGroup()
{
    // Intent interpreter item 1 (owner decision 2026-10-17): one group "Experimental content-adaptive
    // settings", a checkbox per function, default off; content recognition's checkbox writes
    // intent_content on/off and its hold time sits beneath it, greyed while it is off; the other
    // four functions are tested in testIntentRemainderGroup.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer]\nquality: high\n[vprenderer.Sports]\nintent_content_hold_seconds: 8\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* section = requireControl<QToolButton>(window, "rendererSection.contentAdaptive");
    require(section->text() == QStringLiteral("Experimental content-adaptive settings"), "the group heading is wrong");
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* check = requireControl<QCheckBox>(window, "config.vprenderer.intent_content.check");
    auto* mode = requireControl<QComboBox>(window, "config.vprenderer.intent_content");
    auto* hold = requireControl<QLineEdit>(window, "config.vprenderer.intent_content_hold_seconds");
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    require(!check->isChecked(), "content recognition is not off by default");
    require(!hold->isEnabled(), "the hold time is not greyed with content recognition off");
    check->click();
    QCoreApplication::processEvents();
    require(mode->currentData().toString() == "on" && hold->isEnabled(), "the checkbox did not turn content recognition on");
    hold->setText(QStringLiteral("6"));
    QCoreApplication::processEvents();
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    require(saved.contains("intent_content: on") && saved.contains("intent_content_hold_seconds: 6"),
        (QByteArray("content recognition not saved: ") + saved.left(400)).constData());
    // A named profile shows its own hold time and the default profile's (inherited) switch.
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(hold->text() == QStringLiteral("8"), "the named profile's hold time is not shown");
    require(check->isChecked(), "the inherited switch is not shown in the named profile");
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    check->click();
    QCoreApplication::processEvents();
    require(mode->currentData().toString() == "off" && !hold->isEnabled(), "unticking did not turn it off");
}

void testDetailRestoreControls()
{
    // docs/VP-detail-restore.md (2026-10-17): detail_pulse_stabilise (default on), detail_pulse_cap,
    // texture_restore (default off) and texture_restore_cap sit in the "Experimental content-adaptive
    // settings" group, each with a tooltip; the caps are greyed while their stage is off and snap to
    // steps of 0.05 within their range; choices and caps round-trip to the cfg.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer]\nquality: high\n[vprenderer.Film]\ntexture_restore: statistical\ntexture_restore_cap: 1.75\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* content = window.findChild<QWidget*>(QStringLiteral("rendererSection.contentAdaptive.content"));
    require(content != nullptr, "the content-adaptive group's content");
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* pulse = requireControl<QComboBox>(window, "config.vprenderer.detail_pulse_stabilise");
    auto* pulseCap = requireControl<QLineEdit>(window, "config.vprenderer.detail_pulse_cap");
    auto* restore = requireControl<QComboBox>(window, "config.vprenderer.texture_restore");
    auto* restoreCap = requireControl<QLineEdit>(window, "config.vprenderer.texture_restore_cap");
    auto* pulseFast = requireControl<QComboBox>(window, "config.vprenderer.detail_pulse_fast");   // 2026-10-18
    // Organisation passes 2026-10-24: the caps are in VP-V > Repair > Advanced > Limits, the stage
    // switches (the Repair tier's keys) in Advanced > Set by the Repair tier; the fast path (a VP-V
    // toggle on the retirement tab) keeps its hidden row in the content-adaptive group.
    auto* texture = requireControl<QWidget>(window, "vpv.repair.card.limits.body");
    auto* tierKeys = requireControl<QWidget>(window, "vpv.repair.card.tier_keys.body");
    for (QWidget* control : std::initializer_list<QWidget*>{ pulse, pulseCap, pulseFast, restore, restoreCap })
    {
        const bool cap = control == pulseCap || control == restoreCap;
        const bool stage = control == pulse || control == restore;
        require((cap ? texture : stage ? tierKeys : content)->isAncestorOf(control), "a detail control is outside its group");
        require(!control->toolTip().trimmed().isEmpty(), "a detail control has no tooltip");
    }
    require(pulse->toolTip().contains("never averaged") || pulse->toolTip().contains("nothing is copied", Qt::CaseInsensitive),
        "the detail pulse tooltip does not say grain is kept per frame");
    require(restore->toolTip().contains("out-of-focus") && restore->toolTip().contains("nothing is synthesised"),
        "the texture restore tooltip does not say it never adds detail out of focus or synthesises");
    require(pulse->toolTip().contains("source-resolution") && restore->toolTip().contains("source-resolution"),
        "the tooltips do not say source-resolution detection is unchanged");
    require(pulse->toolTip().contains("GPU cost") && restore->toolTip().contains("GPU cost"), "the tooltips have no cost class");
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    auto effectiveChoiceValueForTest = [](const QComboBox* combo) {
        const QString selected = combo->currentData().toString();
        return selected.isEmpty() ? combo->property("inheritedValue").toString().trimmed().toLower() : selected;
    };
    require(effectiveChoiceValueForTest(pulse) == QStringLiteral("on"), "detail pulse is not on by default");
    require(effectiveChoiceValueForTest(restore) == QStringLiteral("off"), "texture restore is not off by default");
    require(effectiveChoiceValueForTest(pulseFast) == QStringLiteral("on") && (pulseFast->isEnabled() || pulseFast->property("vpvManaged").toBool()), "the detail pulse fast path is not on by default");
    require(pulseCap->isEnabled() && !restoreCap->isEnabled(), "the caps are not greyed from the effective stage values");
    require(pulseCap->placeholderText() == QStringLiteral("1.35") && restoreCap->placeholderText() == QStringLiteral("1.50"),
        "the cap placeholders are not the defaults");
    // A named profile shows its own values.
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(restore->currentData().toString() == QStringLiteral("statistical"), "the profile's texture restore is not shown");
    require(restoreCap->text() == QStringLiteral("1.75") && restoreCap->isEnabled(), "the profile's cap is not shown or is greyed");
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    // Round trip: set each, apply, read the cfg.
    pulse->setCurrentIndex(pulse->findData(QStringLiteral("off")));
    restore->setCurrentIndex(restore->findData(QStringLiteral("statistical")));
    QCoreApplication::processEvents();
    require(!pulseCap->isEnabled() && restoreCap->isEnabled(), "the caps did not follow the stage choices");
    require(!pulseFast->isEnabled(), "the fast path choice is not greyed while the pulse is off");
    pulse->setCurrentIndex(pulse->findData(QStringLiteral("on")));
    QCoreApplication::processEvents();
    pulseFast->setCurrentIndex(pulseFast->findData(QStringLiteral("off")));
    pulseCap->setText(QStringLiteral("1.9"));
    pulseCap->editingFinished();
    require(pulseCap->text() == QStringLiteral("1.5"), (QByteArray("the detail pulse cap was not clamped: ") + pulseCap->text().toUtf8()).constData());
    pulseCap->setText(QStringLiteral("1.22"));
    pulseCap->editingFinished();
    require(pulseCap->text() == QStringLiteral("1.2"), (QByteArray("the detail pulse cap did not snap to 0.05: ") + pulseCap->text().toUtf8()).constData());
    restoreCap->setText(QStringLiteral("1.6"));
    restoreCap->editingFinished();
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    require(saved.contains("detail_pulse_cap: 1.2") && saved.contains("texture_restore: statistical") &&
        saved.contains("texture_restore_cap: 1.6") && saved.contains("detail_pulse_fast: off"),
        (QByteArray("detail restore settings not saved: ") + saved.left(500)).constData());
}

void testIntentRemainderGroup()
{
    // Intent remainder (2026-10-17): motion intent (with its five exclusive policies), ambient
    // adaptation, black protection and the display mapping readout, each a checkbox over a hidden
    // drop-down, sub-settings greyed while off, tooltips naming their key.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer]\nquality: high\n# preserve me\n[vprenderer.Sports]\nintent_motion_policy: ease_slow\nambient_tau_up_s: 20\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto tip = [](const QWidget* w, const char* key) {
        require(!w->toolTip().isEmpty() && w->toolTip().contains(QString::fromLatin1(key)),
            (QByteArray("tooltip missing or without its key: ") + key).constData());
    };
    auto* motion = requireControl<QCheckBox>(window, "config.vprenderer.intent_motion.check");
    auto* ambient = requireControl<QCheckBox>(window, "config.vprenderer.ambient_adapt.check");
    auto* black = requireControl<QCheckBox>(window, "config.vprenderer.intent_black.check");
    auto* display = requireControl<QCheckBox>(window, "config.vprenderer.intent_display.check");
    tip(motion, "intent_motion"); tip(ambient, "ambient_adapt"); tip(black, "intent_black"); tip(display, "intent_display");
    require(motion->toolTip().contains("never raises dejudder_strength") && motion->toolTip().contains("experimental", Qt::CaseInsensitive) &&
        motion->toolTip().contains("default off"), "motion tooltip lacks the rules");
    require(ambient->toolTip().contains("[ambient]") && ambient->toolTip().contains("lux_lit") &&
        ambient->toolTip().contains("not dark theatres"), "ambient tooltip lacks the sensor / room statement");
    require(black->toolTip().contains("code 0") && black->toolTip().contains("BTB"), "black tooltip lacks the code 0 / BTB rule");
    const char* policies[] = { "display", "ease_fast", "ease_slow", "visibility", "metadata" };
    QList<QRadioButton*> radios;
    for (const char* p : policies)
    {
        auto* r = requireControl<QRadioButton>(window, QString("config.vprenderer.intent_motion_policy.") + p);
        tip(r, "intent_motion_policy");
        radios.push_back(r);
    }
    require(radios[0]->text().startsWith("Display only") && radios[1]->text().startsWith("Ease on fast pans") &&
        radios[2]->text().startsWith("Keep fast pans, ease on slow deliberate shots") &&
        radios[3]->text().startsWith("Visibility-weighted") && radios[4]->text().startsWith("Metadata only"),
        "policy button labels are wrong");
    const char* numbers[] = { "intent_motion_min", "intent_motion_min_shot_s", "ambient_adapt_strength",
        "ambient_screen_reflect", "ambient_max_black_nits", "ambient_tau_up_s", "ambient_tau_down_s" };
    auto* radioBox = requireControl<QWidget>(window, "config.vprenderer.intent_motion_policy.buttons");
    auto* blackBox = requireControl<QWidget>(window, "config.vprenderer.intent_black.buttons");
    auto* reportRadio = requireControl<QRadioButton>(window, "config.vprenderer.intent_black.report");
    auto* protectRadio = requireControl<QRadioButton>(window, "config.vprenderer.intent_black.led_protect");
    tip(reportRadio, "intent_black"); tip(protectRadio, "intent_black");
    auto* blackCombo = requireControl<QComboBox>(window, "config.vprenderer.intent_black");
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    require(!motion->isChecked() && !ambient->isChecked() && !black->isChecked() && !display->isChecked(),
        "an experimental function is not off by default");
    for (const char* key : numbers)
    {
        auto* edit = requireControl<QLineEdit>(window, QString("config.vprenderer.") + key);
        tip(edit, key);
        require(!edit->isEnabled(), (QByteArray("not greyed while off: ") + key).constData());
    }
    require(!radioBox->isEnabled() && !blackBox->isEnabled(), "the policy / black choice is not greyed while off");
    motion->click(); ambient->click(); black->click(); display->click();
    QCoreApplication::processEvents();
    for (const char* key : numbers)
        require(requireControl<QLineEdit>(window, QString("config.vprenderer.") + key)->isEnabled(),
            (QByteArray("not enabled when on: ") + key).constData());
    require(radioBox->isEnabled() && blackBox->isEnabled(), "the policy / black choice stays greyed when on");
    // The fade model (intent_ease_s): shared, always editable, tooltip names its key.
    {
        auto* ease = requireControl<QLineEdit>(window, "config.vprenderer.intent_ease_s");
        tip(ease, "intent_ease_s");
        require(ease->isEnabled() && ease->placeholderText() == "1.5", "intent_ease_s is not editable / wrong default");
    }
    require(blackCombo->currentData().toString() == "report" && reportRadio->isChecked(),
        "ticking black protection did not select report");
    require(radios[0]->isChecked(), "the default policy is not Display only");
    // Exclusive: choosing one unchecks the others.
    radios[3]->click();
    QCoreApplication::processEvents();
    for (int i = 0; i < 5; ++i) require(radios[i]->isChecked() == (i == 3), "policy buttons are not exclusive");
    protectRadio->click();
    requireControl<QLineEdit>(window, "config.vprenderer.intent_motion_min")->setText("0.4");
    requireControl<QLineEdit>(window, "config.vprenderer.ambient_tau_down_s")->setText("120");
    QCoreApplication::processEvents();
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    QByteArray saved = readBytes(path);
    const QByteArray status = requireControl<QLabel>(window, "configurationStatus")->text().toUtf8();
    for (const char* line : { "intent_motion: on", "intent_motion_policy: visibility", "intent_motion_min: 0.4",
        "ambient_adapt: on", "ambient_tau_down_s: 120", "intent_black: led_protect", "intent_display: report",
        "# preserve me" })
        require(saved.contains(line), (QByteArray("not saved / not preserved [") + status + "]: " + line + " | " + QByteArray(saved.left(600)).replace('\n', ' ')).constData());
    // Only the edited keys were added; untouched ones were not written.
    for (const char* absent : { "intent_motion_min_shot_s", "ambient_adapt_strength", "ambient_screen_reflect",
        "ambient_max_black_nits" })
        require(!saved.contains(absent), (QByteArray("unedited key written: ") + absent).constData());
    // Black off / report / led_protect through the controls.
    black->click();
    QCoreApplication::processEvents();
    require(blackCombo->currentData().toString() == "off" && !blackBox->isEnabled(),
        "unticking black protection did not turn it off");
    black->click();
    reportRadio->click();
    QCoreApplication::processEvents();
    require(blackCombo->currentData().toString() == "report", "report not selected");
    // A named profile: its own policy and tau, the default profile's switches inherited.
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(radios[2]->isChecked(), "the named profile policy is not shown");
    require(requireControl<QLineEdit>(window, "config.vprenderer.ambient_tau_up_s")->text() == "20",
        "the profile tau is not shown");
    require(motion->isChecked(), "the inherited motion switch is not shown in the named profile");
    radios[4]->click();
    QCoreApplication::processEvents();
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    saved = readBytes(path);
    require(saved.contains("intent_motion_policy: metadata") && saved.contains("ambient_tau_up_s: 20") &&
        saved.contains("# preserve me"),
        (QByteArray("profile round trip failed: ") + saved.left(800)).constData());
}

void testInheritedButtonsAndReorganisedSections()
{
    // Audit 2026-10-16: an unset profile's exclusive buttons show the default profile's choice,
    // marked inherited; clicking it writes it; "Not set" removes it again. And VP's settings sit in
    // their signal-order sections (Capture & source, Source resolution & re-base).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.Cinema]\noptical_flow: off\n[vprenderer.Other]\nquality: high\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto inSection = [&window](const char* control, const QString& section) {
        auto* widget = window.findChild<QWidget*>(QString::fromLatin1(control));
        auto* content = window.findChild<QWidget*>(QStringLiteral("rendererSection.%1.content").arg(section));
        return widget && content && content->isAncestorOf(widget);
    };
    require(inSection("config.vprenderer.capture_path", QStringLiteral("captureSource")), "capture_path not under Capture & source");
    // Organisation pass 2 (2026-10-24): the picture slider is the master on VP-V > Scaling > Main, VP
    // deband a Repair tier key (VP-V > Repair > Advanced > Set by the Repair tier).
    auto inCard = [&window](const char* control, const char* card) {
        auto* widget = window.findChild<QWidget*>(QString::fromLatin1(control));
        auto* body = window.findChild<QWidget*>(QString::fromLatin1(card));
        return widget && body && body->isAncestorOf(widget);
    };
    require(inCard("config.vprenderer.picture_preset_box", "vpv.scaling.card.rebase.body"), "Picture not the Scaling master");
    require(inCard("config.vprenderer.deband_vp", "vpv.repair.card.tier_keys.body"), "VP deband not set by the Repair tier");
    require(inSection("config.vprenderer.ssim_residual_stabilise", QStringLiteral("adaptiveDownscale")), "SSIM steadiness not with the downscale blend");
    // Pruning 2026-10-17: the retired switches have no control any more.
    for (const char* retired : { "config.vprenderer.dither_vectors_new_frame", "config.vprenderer.fix_scene_cooldown",
            "config.vprenderer.dejudder_picture_area" })
        require(!window.findChild<QWidget*>(QString::fromLatin1(retired)), "a retired switch still has a control");
    require(inSection("config.vprenderer.native_rgb_ingest", QStringLiteral("fixesUnderTest")), "native RGB left Fixes under test");
    require(inSection("config.vprenderer.dv_tunnel_gpu_unpack", QStringLiteral("fixesUnderTest")), "DV GPU unpack left Fixes under test");
    require(inSection("config.vprenderer.dv_trims", QStringLiteral("fixesUnderTest")), "DV trims left Fixes under test");
    require(inSection("config.vprenderer.capture_upload", QStringLiteral("fixesUnderTest")), "capture_upload left Fixes under test");
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    auto* matcher = requireControl<QRadioButton>(window, "config.vprenderer.optical_flow.matcher");
    auto* value = requireControl<QComboBox>(window, "config.vprenderer.optical_flow");
    auto* unset = requireControl<QToolButton>(window, "config.vprenderer.optical_flow.unset");
    require(matcher->isChecked(), "the inherited matcher-only is not shown");
    require(matcher->text().contains("(inherited)"), (QByteArray("not marked inherited: ") + matcher->text().toUtf8()).constData());
    require(value->currentData().toString().isEmpty(), "the profile should be unset");
    require(matcher->isEnabled(), "the motion source buttons are disabled");
    matcher->click();
    QCoreApplication::processEvents();
    require(value->currentData().toString() == "off", "a click on the shown inherited button did not write it");
    require(!matcher->text().contains("(inherited)"), "still marked inherited after the click");
    unset->click();
    QCoreApplication::processEvents();
    require(value->currentData().toString().isEmpty() && matcher->isChecked(), "Not set did not return to inherited");
}

void testQueueTargetByFramesOrTime()
{
    // Owner 2026-10-16: the queue target is by frames or by time - exclusive buttons, one field shown;
    // by frames writes the time off, by time writes a delay.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n[queue.Timed]\ntarget_latency_ms: 300\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.queue.profiles");
    auto* byFrames = requireControl<QRadioButton>(window, "config.queue.target_mode.frames");
    auto* byTime = requireControl<QRadioButton>(window, "config.queue.target_mode.time");
    auto* latency = requireControl<QSpinBox>(window, "config.queue.target_latency_ms");
    profiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    require(byFrames->isChecked() && !byTime->isChecked(), "a frames queue is not shown as By frames");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(byTime->isChecked() && latency->value() == 300, "a timed queue is not shown as By time with its delay");
    byFrames->click();
    QCoreApplication::processEvents();
    require(latency->value() == 0 && !byTime->isChecked(), "By frames did not switch the time off");
    byTime->click();
    QCoreApplication::processEvents();
    require(latency->value() == 200, "By time did not start at 200 ms");
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("target_latency_ms: 200"), "the time target not saved");
}

void testQueueUseModeAndLowLatencyPath()
{
    // Owner 2026-10-16: when a queue is used - Off / By rules / 50/60 Hz - and its low-latency path.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n[queue.Game]\nqueue_size: 2\ntarget_frames: 1\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.queue.profiles");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    auto* off = requireControl<QRadioButton>(window, "config.queue.use_mode.off");
    auto* rules = requireControl<QRadioButton>(window, "config.queue.use_mode.rules");
    auto* rate = requireControl<QRadioButton>(window, "config.queue.use_mode.rate5060");
    require(off->isChecked(), "a queue without a rule is not shown as Off");
    rate->click();
    QCoreApplication::processEvents();
    require(rate->isChecked() && !off->isChecked(), "50/60 Hz not selected");
    rules->click();
    QCoreApplication::processEvents();
    require(rules->isChecked(), "By rules not selected");
    rate->click();
    QCoreApplication::processEvents();
    selectData(requireControl<QComboBox>(window, "config.queue.low_latency_path"), "on");
    QCoreApplication::processEvents();
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    // Fixed-constant batch 2026-10-18: the class rule (50 Hz and faster), not the fixed 49-61 band.
    require(saved.contains("${cadence} >= 49"), (QByteArray("the video-rate rule not saved: ") + saved).constData());
    require(!saved.contains("49-61"), "the fixed 49-61 band written");
    require(saved.contains("low_latency_path: on"), "low_latency_path not saved");
    {
        // A queue saved with the earlier 49-61 rule still reads as the rate button, kept as written.
        QFile legacy(path); require(legacy.open(QIODevice::WriteOnly), "legacy fixture");
        legacy.write("[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n[queue.Game]\nqueue_size: 2\ntarget_frames: 1\n"
            "when: ${cadence} == 49-61\n");
        legacy.close();
        ConfigEditorWindow reopened(path, 0, true);
        auto* reopenedProfiles = requireControl<QListWidget>(reopened, "config.queue.profiles");
        reopenedProfiles->setCurrentRow(1);
        QCoreApplication::processEvents();
        require(requireControl<QRadioButton>(reopened, "config.queue.use_mode.rate5060")->isChecked(),
            "a saved 49-61 rule is not shown as the rate button");
        require(!requireControl<QRadioButton>(reopened, "config.queue.use_mode.rules")->isChecked(),
            "a saved 49-61 rule is shown as By rules");
    }
}

void testDeckLinkYCbCr12Control()
{
    // Planner 2026-10-16: [decklink] ycbcr_12bit_capture has a Config control; unset shows the default.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[general]\ncapture_device: BlackMagic DeckLink\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* control = requireControl<QComboBox>(window, "config.decklink.ycbcr_12bit_capture");
    require(control->currentData().toString() == "v210", (QByteArray("unset is not shown as the default v210 (bug hunt 2): ") +
        control->currentData().toString().toUtf8()).constData());
    selectData(control, "rgb12");
    QCoreApplication::processEvents();
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    require(apply->isEnabled(), "the edit did not enable Apply");
    apply->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    require(saved.contains("[decklink]") && saved.contains("ycbcr_12bit_capture: rgb12"),
        ("ycbcr_12bit_capture not saved: " + requireControl<QLabel>(window, "configurationStatus")->text()).toStdString().c_str());
    ConfigEditorWindow reopened(path, 0, true);
    require(requireControl<QComboBox>(reopened, "config.decklink.ycbcr_12bit_capture")->currentData().toString() == "rgb12",
        "the reopened Config does not show rgb12 (the earlier choice stays restorable)");
}

void testMotionSliderPerRuleProfile()
{
    // Owner 2026-10-16: rules give each frame rate its own level - each rule-chosen profile keeps its own
    // slider position (a 24p profile at Cinema, a 30p profile at Natural).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.Cinema]\nquality: high\n[vprenderer.Doc30]\nwhen: $source_rate > 29\nquality: high\n"
        "[vprenderer.Film24]\nwhen: $source_rate < 25\nquality: high\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* slider = requireControl<QSlider>(window, "config.vprenderer.motion_preset");
    auto rowOf = [profiles](const char* text) {
        for (int i = 0; i < profiles->count(); ++i)
            if (profiles->item(i)->text().contains(text)) return i;
        return -1;
    };
    if (rowOf("Doc30") < 0 || rowOf("Film24") < 0)
    {
        QByteArray listed;
        for (int i = 0; i < profiles->count(); ++i) listed += "'" + profiles->item(i)->text().toUtf8() + "' ";
        require(false, (QByteArray("rule profiles listed: ") + listed).constData());
    }
    profiles->setCurrentRow(rowOf("Doc30"));
    QCoreApplication::processEvents();
    slider->setValue(3);
    profiles->setCurrentRow(rowOf("Film24"));
    QCoreApplication::processEvents();
    require(requireControl<QLabel>(window, "config.vprenderer.motion_preset_name")->text().startsWith("Not set"),
        "The other profile is not independent");
    slider->setValue(2);
    QCoreApplication::processEvents();
    profiles->setCurrentRow(rowOf("Doc30"));
    QCoreApplication::processEvents();
    require(slider->value() == 3, "Switching back lost the 30p profile's Natural");
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    const int doc = saved.indexOf("[vprenderer.Doc30]"), film = saved.indexOf("[vprenderer.Film24]");
    require(doc >= 0 && film >= 0, "sections saved");
    const QByteArray docBlock = doc < film ? saved.mid(doc, film - doc) : saved.mid(doc);
    const QByteArray filmBlock = film < doc ? saved.mid(film, doc - film) : saved.mid(film);
    require(docBlock.contains("dejudder_strength_even: 0.5") && docBlock.contains("dejudder_strength: 0.4") &&
        docBlock.contains("when: $source_rate > 29"), "The 30p profile did not save Natural with its rule");
    require(filmBlock.contains("dejudder_strength: 0.25") && !filmBlock.contains("dejudder_strength_even: 0.5") &&
        filmBlock.contains("when: $source_rate < 25"), "The 24p profile did not save Cinema with its rule");
}

void testLedDrivePageAndLiveSwitches()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.First]\nquality: high\n"
        "[vprenderer.zoom]\nlabel: Off (whole picture)\nautomatic_crop: false\n"
        "[vprenderer.zoom.black_bars]\nlabel: Black bars removed\nautomatic_crop: true\n"
        "[shader.standard]\ntype: multi\nlabel: Off\n"
        "[shader.standard.sigma_delta]\nshader_type: custom\nlabel: Temporal sigma-delta\n"
        "glsl_file: Temporal sigma-delta.glsl\n"
        "[led_drive.Default]\nenabled: false\nmode: log_only\n"
        "[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);

    // Dynamic Drive: real controls, saved to the configuration on Apply.
    window.selectPage(20);
    auto* enabled = requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.enabled"));
    require(!enabled->isChecked(), "Dynamic Drive did not load as off");
    // The LED core (owner 2026-10-14): the command source is read from an
    // older configuration's mode (log_only = no LED commands).
    auto* mode = requireControl<QComboBox>(window, QStringLiteral("config.led_drive.default.command_source"));
    require(mode->currentData().toString() == QStringLiteral("none"), "The LED command source did not load from mode");
    // Everything greys while Dynamic Drive is off (audit 2026-10-14c).
    require(!mode->isEnabled(), "The command source is offered with Dynamic Drive off");
    enabled->setChecked(true);
    require(mode->isEnabled(), "Switching Dynamic Drive on did not offer the command source");
    require(!requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.lookahead"))->isVisibleTo(&window) &&
        !requireControl<QComboBox>(window, QStringLiteral("config.led_drive.default.script_pacing"))->isVisibleTo(&window),
        "Look-ahead or script pacing is shown with no command source");
    // Only what the chosen source uses (audit 2026-10-14b): with no source,
    // neither compensate's settings nor the script copy's pacing show.
    require(!requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.led_floor"))->isVisibleTo(&window) &&
        !requireControl<QComboBox>(window, QStringLiteral("config.led_drive.default.highlight_tolerance_stops"))->isVisibleTo(&window),
        "Compensate's settings show with no command source");
    selectData(mode, QStringLiteral("internal_script"));
    require(requireControl<QComboBox>(window, QStringLiteral("config.led_drive.default.script_pacing"))->isVisibleTo(&window) &&
        requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.script_measure_ms"))->isVisibleTo(&window),
        "VP's copy of the script does not show its pacing");
    // Owner 2026-10-17: script_lookahead - the script copy's look-ahead switch (on) and the
    // floors it uses show; compensate's own settings do not.
    {
        auto* scriptAhead = requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.script_lookahead"));
        require(scriptAhead->isVisibleTo(&window) && scriptAhead->isChecked(),
            "The script copy's look-ahead is not offered (on)");
        require(requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.led_floor"))->isVisibleTo(&window) &&
            requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.led_floor_reactive"))->isVisibleTo(&window),
            "The floors the script copy uses with look-ahead are not shown");
        require(!requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.led_floor_black"))->isVisibleTo(&window) &&
            !requireControl<QComboBox>(window, QStringLiteral("config.led_drive.default.highlight_tolerance_stops"))->isVisibleTo(&window),
            "Compensate's settings show for VP's copy of the script");
        require(!requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.lookahead"))->isVisibleTo(&window),
            "Compensate's look-ahead switch shows for VP's copy of the script");
    }
    auto* boost = requireControl<QComboBox>(window, QStringLiteral("config.led_drive.default.gamma_boost"));
    auto* port = requireControl<QLineEdit>(window, QStringLiteral("config.projector.default.port"));
    require(port->text() == QStringLiteral("COM1"), "COM port did not load");
    auto* readBack = requireControl<QCheckBox>(window, QStringLiteral("config.projector.default.read_back"));
    // RS-232 connection test button (Dynamic Drive page), idle until pressed.
    auto* connectionTest = requireControl<QPushButton>(window,
        QStringLiteral("config.projector.default.connection_test"));
    require(connectionTest->isEnabled(), "The RS-232 connection test button is disabled");
    require(requireControl<QLabel>(window, QStringLiteral("config.projector.default.connection_test.result"))->text().isEmpty(),
        "The RS-232 test result is not empty before a test");
    require(!readBack->isChecked(), "Read-back did not default to off");
    auto* floor = requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.led_floor"));
    auto* floorBlack = requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.led_floor_black"));
    require(floor->value() == 100 && floor->minimum() == 60 && floor->maximum() == 255,
        "Lowest LED level is not 60-255 with the script's 100");
    require(floorBlack->value() == 60 && floorBlack->minimum() == 60,
        "Fade-to-black level does not default to its minimum, 60");
    floor->setValue(80);
    require(floorBlack->maximum() == 80, "The fade-to-black level may exceed the lowest LED level");
    auto* floorReactive = requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.led_floor_reactive"));
    require(floorReactive->value() == 100 && floorReactive->minimum() == 60,
        "The reactive floor does not default to the script's 100");
    floorReactive->setValue(110);
    auto* latency = requireControl<QSpinBox>(window, QStringLiteral("config.projector.default.led_latency_ms"));
    require(latency->value() == 80, "LED response time does not default to 80 ms");
    latency->setValue(100);
    auto* contrastLatency = requireControl<QSpinBox>(window, QStringLiteral("config.projector.default.cntr_latency_ms"));
    require(contrastLatency->value() == 80, "Contrast response time does not default to 80 ms");
    contrastLatency->setValue(120);
    auto* smoothing = requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.adaptive_gamma"));
    // An older log_only file with no shadow lift: nothing adapted, so off (audit 2026-10-14c).
    require(!smoothing->isChecked() && smoothing->isEnabled(), "Adaptive gamma does not show off for an older log_only file");
    smoothing->setChecked(true);
    // Stage 7 step 7c: every new LED setting has a control.
    auto* cutLevel = requireControl<QDoubleSpinBox>(window, QStringLiteral("config.led_drive.default.cut_cntr_level"));
    require(cutLevel->value() == cutLevel->minimum() && cutLevel->specialValueText().contains(QStringLiteral("auto")),
        "Contrast on a bright cut does not default to auto (the white clip)");
    cutLevel->setValue(57);
    auto* dip = requireControl<QDoubleSpinBox>(window, QStringLiteral("config.led_drive.default.max_cut_dip"));
    require(std::abs(dip->value() - 0.5) < 1e-9, "Darkest start of a fade-up does not default to 0.5");
    dip->setValue(0.4);
    auto* lift = requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.black_prelift_max"));
    require(lift->value() == 120, "The LED lift while black does not default to 120");
    lift->setValue(110);
    auto* rateMaxUp = requireControl<QDoubleSpinBox>(window, QStringLiteral("config.led_drive.default.rate_max_up"));
    require(rateMaxUp->value() == rateMaxUp->minimum(), "An unset speed does not show \"preset\"");
    rateMaxUp->setValue(20.0);
    selectData(requireControl<QComboBox>(window, QStringLiteral("config.led_drive.default.rate_preset")),
        QStringLiteral("compensated"));
    auto* settle = requireControl<QDoubleSpinBox>(window, QStringLiteral("config.projector.default.led_settle_ms"));
    require(std::abs(settle->value() - 300.0) < 1e-9, "LED settle time does not default to 300 ms");
    settle->setValue(250);
    auto* clip = requireControl<QDoubleSpinBox>(window, QStringLiteral("config.projector.default.dmd_clip_cntr"));
    require(std::abs(clip->value() - 59.0) < 1e-9, "White clip does not default to 59");
    clip->setValue(61);
    for (const char* name : { "config.led_drive.default.cntr_assist", "config.led_drive.default.software_first",
        "config.led_drive.default.cut_cntr_dump", "config.led_drive.default.shaped_recovery" })
        require(requireControl<QCheckBox>(window, QString::fromLatin1(name))->isChecked(),
            QStringLiteral("%1 is not on by default").arg(QString::fromLatin1(name)).toStdString().c_str());
    for (const char* name : { "config.led_drive.default.cntr_assist_max", "config.led_drive.default.recovery_bias_pct",
        "config.led_drive.default.rate_gain_up", "config.led_drive.default.rate_min_up",
        "config.led_drive.default.rate_gain_down", "config.led_drive.default.rate_min_down",
        "config.led_drive.default.rate_max_down", "config.led_drive.default.recovery_rate_gain",
        "config.led_drive.default.recovery_rate_min", "config.led_drive.default.recovery_rate_max",
        "config.projector.default.cntr_settle_ms", "config.projector.default.cntr_max_usable" })
        requireControl<QDoubleSpinBox>(window, QString::fromLatin1(name));
    auto* profile = requireControl<QLineEdit>(window, QStringLiteral("config.led_drive.default.profile_file"));
    require(profile->text().isEmpty(), "A profile file appeared from nowhere");
    auto* profileStatus = requireControl<QLabel>(window, QStringLiteral("config.led_drive.default.profile_status"));
    require(profileStatus->isHidden(), "A profile status shows with no profile");
    profile->setText(QStringLiteral("my-led.cfg"));
    require(!profileStatus->isHidden() && profileStatus->text().contains(QStringLiteral("ignore")),
        "A missing profile file is not reported on the page");
    requireControl<QPushButton>(window, QStringLiteral("config.led_drive.default.profile_builtin"))->click();
    require(profile->text().isEmpty(), "Use built-in did not clear the profile file");
    require(profileStatus->isHidden(), "The profile status stays after Use built-in");
    enabled->setChecked(true);
    selectData(mode, QStringLiteral("compensate"));
    require(floor->isVisibleTo(&window) && requireControl<QComboBox>(window,
        QStringLiteral("config.led_drive.default.highlight_tolerance_stops"))->isVisibleTo(&window) &&
        !requireControl<QComboBox>(window, QStringLiteral("config.led_drive.default.script_pacing"))->isVisibleTo(&window),
        "Compensate does not show its own settings (or still shows the script pacing)");
    auto* lookahead = requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.lookahead"));
    require(lookahead->isVisibleTo(&window) && lookahead->isEnabled() && lookahead->isChecked(),
        "Look-ahead is not offered (on) for Compensate");
    // Compensate with adaptive gamma off says what it does.
    auto* warning = requireControl<QLabel>(window, QStringLiteral("config.led_drive.default.compensate_warning"));
    require(!warning->isVisibleTo(&window), "The compensate warning shows with adaptive gamma on");
    lookahead->setChecked(false);
    // The shadow lift does nothing without adaptive gamma: greyed then.
    auto* shadowLiftBox = requireControl<QDoubleSpinBox>(window, QStringLiteral("config.led_drive.default.shadow_lift_ansi"));
    require(shadowLiftBox->isEnabled(), "The shadow lift is greyed with adaptive gamma on");
    smoothing->setChecked(false);
    require(!shadowLiftBox->isEnabled(), "The shadow lift is not greyed with adaptive gamma off");
    smoothing->setChecked(true);
    selectData(boost, QStringLiteral("2.4"));
    auto* projectorGamma = requireControl<QDoubleSpinBox>(window, QStringLiteral("config.led_drive.default.compensation_eotf"));
    require(std::abs(projectorGamma->value() - 2.41) < 1e-9 && projectorGamma->decimals() == 2 &&
        projectorGamma->minimum() <= 1.8 && projectorGamma->maximum() >= 3.0,
        "Projector gamma is not a 2-decimal number from 1.8 to 3.0 defaulting to 2.41 (delta 4)");
    projectorGamma->setValue(2.35); // a measured value, not a fixed choice
    port->setText(QStringLiteral("COM3"));
    readBack->setChecked(true);
    // Experimental fade to black: off, its options greyed out until switched on.
    auto* experiment = requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.ftb_experimental"));
    auto* method = requireControl<QComboBox>(window, QStringLiteral("config.led_drive.default.ftb_custom_dim"));
    auto* stepMs = requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.ftb_step_ms"));
    auto* customFloor = requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.ftb_custom_floor"));
    require(!experiment->isChecked() && !method->isEnabled() && !stepMs->isEnabled() && !customFloor->isEnabled(),
        "The experimental fade-to-black options are not greyed out while it is off");
    require(stepMs->value() == 100, "The step interval does not default to 100 ms");
    experiment->setChecked(true);
    require(method->isEnabled() && stepMs->isEnabled() && customFloor->isEnabled(),
        "Switching the experiment on did not enable its options");
    auto* turnStep = requireControl<QSpinBox>(window, QStringLiteral("config.led_drive.default.ftb_turn_step"));
    selectData(method, QStringLiteral("together"));
    require(!turnStep->isEnabled(), "The step size is offered for 'together', where it does nothing");
    selectData(method, QStringLiteral("turns"));
    require(turnStep->isEnabled() && turnStep->value() == 2, "The step size is not offered for 'taking turns'");
    turnStep->setValue(3);
    customFloor->setValue(20);
    // Bug hunt 2 (owner 2026-10-17): the peak from every pixel and picture-only measurement,
    // restore switches on by default with their own controls.
    auto* fullPeak = requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.led_peak_full_coverage"));
    auto* pictureOnly = requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.led_picture_only"));
    require(fullPeak->isChecked() && pictureOnly->isChecked(),
        "led_peak_full_coverage / led_picture_only are not on by default");
    // Organisation pass 2: the LED fixes under test have one master (Advanced LED card), all on until Custom.
    require(!fullPeak->isEnabled(), "the LED fixes are not set by their master");
    requireControl<QCheckBox>(window, "vpv.drive.led_fixes")->setChecked(true);
    QCoreApplication::processEvents();
    require(fullPeak->isVisibleTo(&window) && pictureOnly->isVisibleTo(&window) && fullPeak->isEnabled() &&
        pictureOnly->isEnabled(), "The Dynamic Drive measurement switches are not offered");
    require(fullPeak->toolTip().contains(QStringLiteral("Dynamic Drive")) &&
        pictureOnly->toolTip().contains(QStringLiteral("subtitle")), "The measurement switches do not say what they do");
    fullPeak->setChecked(false);
    pictureOnly->setChecked(false);
    QCoreApplication::processEvents();
    QPushButton* apply = requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"));
    require(apply->isEnabled(), "Dynamic Drive edits did not enable Apply");
    apply->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    for (const char* expected : { "enabled: true", "mode: compensate", "gamma_boost: 2.4",
        "port: COM3", "read_back: true", "led_floor: 80", "ftb_custom_dim: turns", "ftb_custom_floor: 20",
        "led_floor_reactive: 110", "led_latency_ms: 100", "ftb_turn_step: 3", "compensation_eotf: 2.35",
        "cntr_latency_ms: 120", "adaptive_gamma: true", "command_source: compensate", "lookahead: false", "cut_cntr_level: 57", "max_cut_dip: 0.40",
        "black_prelift_max: 110", "rate_max_up: 20.0", "rate_preset: compensated", "led_settle_ms: 250",
        "dmd_clip_cntr: 61", "led_peak_full_coverage: false", "led_picture_only: false" })
        require(saved.contains(expected), QStringLiteral("Dynamic Drive setting not saved: %1")
            .arg(QString::fromLatin1(expected)).toStdString().c_str());
    experiment->setChecked(false);
    require(!method->isEnabled(), "Switching the experiment off did not grey out its options");
    QCoreApplication::processEvents();
    apply->click();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("ftb_custom_dim: off"), "Switching the experiment off was not saved");

    // Live switches: visible buttons next to the shader and zoom lists.
    auto* shaders = requireControl<QListWidget>(window, QStringLiteral("config.shader.standard.items"));
    QListWidgetItem* sigma = nullptr;
    for (int row = 0; row < shaders->count(); ++row)
        if (shaders->item(row)->data(Qt::UserRole).toString().compare(
            QStringLiteral("shader.standard.sigma_delta"), Qt::CaseInsensitive) == 0)
            sigma = shaders->item(row);
    require(sigma != nullptr, "Shader fixture missing");
    shaders->setCurrentItem(sigma);
    auto* shaderSwitch = requireControl<QPushButton>(window, QStringLiteral("config.shader.standard.live_toggle"));
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, {});
    QCoreApplication::processEvents();
    require(shaderSwitch->text() == QStringLiteral("Switch on in VP"),
        "An inactive shader does not offer Switch on in VP");
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, { QStringLiteral("shader.standard.sigma_delta") });
    QCoreApplication::processEvents();
    require(shaderSwitch->text() == QStringLiteral("Switch off in VP"),
        "An active shader does not offer Switch off in VP");

    auto* zoomProfiles = requireControl<QListWidget>(window, QStringLiteral("config.vprenderer.zoom.profiles"));
    QListWidgetItem* blackBars = nullptr;
    for (int row = 0; row < zoomProfiles->count(); ++row)
        if (zoomProfiles->item(row)->data(Qt::UserRole).toString().compare(
            QStringLiteral("vprenderer.zoom.black_bars"), Qt::CaseInsensitive) == 0)
            blackBars = zoomProfiles->item(row);
    require(blackBars != nullptr, "Zoom fixture missing");
    zoomProfiles->setCurrentItem(blackBars);
    auto* zoomSwitch = requireControl<QPushButton>(window, QStringLiteral("config.vprenderer.zoom.live_toggle"));
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, {}, true, QStringLiteral("vprenderer.zoom"));
    QCoreApplication::processEvents();
    require(zoomSwitch->text() == QStringLiteral("Use in VP") && zoomSwitch->isEnabled(),
        "An inactive zoom profile does not offer Use in VP");
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, {}, true, QStringLiteral("vprenderer.zoom.black_bars"));
    QCoreApplication::processEvents();
    require(zoomSwitch->text() == QStringLiteral("In use in VP") && !zoomSwitch->isEnabled(),
        "The active zoom profile is not shown as in use");
}

void testQueueFrameRateRuleIsAPlainControl()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.First]\nquality: high\n"
        "[queue.Normal]\nqueue_size: 32\ntarget_frames: 3\n"
        "[queue.Low_Latency]\nwhen: ${source_rate} > 59\nqueue_size: 1\ntarget_frames: 1\n"
        "[queue.Hdr]\nwhen: ${eotf} == \"HDR\"\nqueue_size: 8\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(1);
    auto* profiles = requireControl<QListWidget>(window, QStringLiteral("config.queue.profiles"));
    auto select = [profiles](const QString& section)
    {
        for (int row = 0; row < profiles->count(); ++row)
            if (profiles->item(row)->data(Qt::UserRole).toString().compare(section, Qt::CaseInsensitive) == 0)
            {
                profiles->setCurrentRow(row);
                QCoreApplication::processEvents();
                return true;
            }
        return false;
    };
    auto* rateRule = requireControl<QCheckBox>(window, QStringLiteral("config.queue.rate_rule"));
    auto* rateFps = requireControl<QDoubleSpinBox>(window, QStringLiteral("config.queue.rate_rule_fps"));
    auto* rule = requireControl<QPlainTextEdit>(window, QStringLiteral("config.queue.when"));

    require(select(QStringLiteral("queue.low_latency")), "Low_Latency missing");
    require(rateRule->isChecked() && std::abs(rateFps->value() - 59.0) < 1e-9,
        "The above-59 rule does not show as the frame-rate switch");
    rateFps->setValue(50.0);
    QCoreApplication::processEvents();
    require(rule->toPlainText() == QStringLiteral("${cadence} > 50"),
        "Changing the frame rate did not rewrite the rule");

    require(select(QStringLiteral("queue.normal")), "Normal missing");
    require(!rateRule->isChecked() && rateRule->isEnabled(), "A queue without a rule shows the switch off");

    require(select(QStringLiteral("queue.hdr")), "Hdr missing");
    require(!rateRule->isChecked() && !rateRule->isEnabled(),
        "A custom formula is not left alone by the frame-rate switch");
    require(requireControl<QLabel>(window, QStringLiteral("config.queue.rate_rule_note"))->isVisibleTo(&window),
        "A custom formula does not explain why the switch is off");

    QPushButton* apply = requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"));
    apply->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    require(saved.contains("when: ${cadence} > 50"), "The new frame-rate rule was not saved");
    require(saved.contains("when: ${eotf} == \"HDR\""), "The custom rule was changed");

    require(select(QStringLiteral("queue.low_latency")), "Low_Latency missing");
    rateRule->setChecked(false);
    QCoreApplication::processEvents();
    apply->click();
    QCoreApplication::processEvents();
    require(!readBytes(path).contains("source_rate") && !readBytes(path).contains("${cadence}"), "Switching the frame-rate rule off did not remove it");
}

void testQueueTargetDelayIsOnTheQueuePage()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.First]\nquality: high\n"
        "[queue.Normal]\nqueue_size: 32\ntarget_frames: 3\ntarget_latency_ms: 120\n"
        "[queue.Low_Latency]\nwhen: ${source_rate} > 59\nqueue_size: 1\ntarget_frames: 1\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(1);
    auto* profiles = requireControl<QListWidget>(window, QStringLiteral("config.queue.profiles"));
    auto select = [profiles](const QString& section)
    {
        for (int row = 0; row < profiles->count(); ++row)
            if (profiles->item(row)->data(Qt::UserRole).toString().compare(section, Qt::CaseInsensitive) == 0)
            {
                profiles->setCurrentRow(row);
                QCoreApplication::processEvents();
                return true;
            }
        return false;
    };
    auto* delay = requireControl<QSpinBox>(window, QStringLiteral("config.queue.target_latency_ms"));
    auto* note = requireControl<QLabel>(window, QStringLiteral("config.queue.target_latency_note"));
    require(select(QStringLiteral("queue.normal")), "Normal missing");
    require(delay->value() == 120, "The saved delay is not shown");
    require(note->text().contains(QStringLiteral("24p 3 frames (125 ms)")) &&
        note->text().contains(QStringLiteral("60p 8 frames (133 ms)")),
        "The note does not show the real delay in whole frames");
    require(select(QStringLiteral("queue.low_latency")), "Low_Latency missing");
    require(delay->value() == 0 && delay->text() == QStringLiteral("Off"),
        "A queue without a delay does not show Off");
    require(note->text().startsWith(QStringLiteral("Off")), "The Off note is missing");

    require(select(QStringLiteral("queue.normal")), "Normal missing");
    delay->setValue(200);
    QCoreApplication::processEvents();
    require(note->text().contains(QStringLiteral("24p 5 frames (209 ms)")), "The note does not follow the value");
    QPushButton* apply = requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"));
    apply->click();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("target_latency_ms: 200"), "The new delay was not saved");

    // A buffering preset is a frame-count baseline: it switches the delay off.
    auto* policy = requireControl<QComboBox>(window, QStringLiteral("config.queue.policy_preset"));
    policy->setCurrentIndex(policy->findData(QStringLiteral("reliable")));
    QCoreApplication::processEvents();
    require(delay->value() == 0 && note->text().startsWith(QStringLiteral("Off")),
        "A preset left the delay on");
    require(policy->currentData().toString() == QStringLiteral("reliable"), "The preset did not stick");
    apply->click();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("target_latency_ms: 0"), "The preset did not switch the delay off in the file");
}

void testActivePictureLookaheadIsAutomaticWhenAbsent()
{
    // Owner decision 2026-10-17: absent = automatic (follows the queue); an explicit value, including 0, overrides.
    QTemporaryDir dir;
    const QString path = dir.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.First]\nquality: high\n"
        "[queue.Normal]\nqueue_size: 32\ntarget_frames: 5\n"
        "[queue.Other]\nqueue_size: 16\ntarget_frames: 4\nactive_picture_lookahead_frames: 3\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(1);
    auto* profiles = requireControl<QListWidget>(window, QStringLiteral("config.queue.profiles"));
    auto select = [profiles](const QString& section)
    {
        for (int row = 0; row < profiles->count(); ++row)
            if (profiles->item(row)->data(Qt::UserRole).toString().compare(section, Qt::CaseInsensitive) == 0)
            {
                profiles->setCurrentRow(row);
                QCoreApplication::processEvents();
                return true;
            }
        return false;
    };
    auto* spin = requireControl<QSpinBox>(window, QStringLiteral("config.queue.active_picture_lookahead_frames"));
    auto* apply = requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"));
    require(spin->toolTip().contains("(Key: active_picture_lookahead_frames.)") &&
        spin->toolTip().contains("min(target frames - 1, queue size - 1, 8)") && spin->toolTip().contains("low-latency profile uses 0"),
        "the tooltip does not state the automatic rule and key");
    require(select(QStringLiteral("queue.other")) && spin->value() == 3, "an explicit value is not shown");
    require(select(QStringLiteral("queue.normal")) && spin->value() == spin->minimum() && spin->text() == QStringLiteral("Auto"),
        "an absent key does not show Auto");
    apply->click();
    QCoreApplication::processEvents();
    require(!readBytes(path).contains("[queue.Other]\nactive_picture_lookahead_frames") &&
        readBytes(path).count("active_picture_lookahead_frames") == 1, "saving wrote the key for an Auto profile");
    spin->setValue(0);
    QCoreApplication::processEvents();
    apply->click();
    QCoreApplication::processEvents();
    require(readBytes(path).count("active_picture_lookahead_frames: 0") == 1, "an explicit 0 is not saved");
    spin->setValue(6);
    QCoreApplication::processEvents();
    apply->click();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("active_picture_lookahead_frames: 6"), "an explicit value does not round-trip");
    spin->setValue(spin->minimum());
    QCoreApplication::processEvents();
    apply->click();
    QCoreApplication::processEvents();
    require(readBytes(path).count("active_picture_lookahead_frames") == 1, "returning to Auto did not remove the key");
    // A buffering preset no longer writes it; it removes an explicit value.
    require(select(QStringLiteral("queue.normal")), "Normal missing");
    auto* policy = requireControl<QComboBox>(window, QStringLiteral("config.queue.policy_preset"));
    policy->setCurrentIndex(policy->findData(QStringLiteral("reliable")));
    QCoreApplication::processEvents();
    require(spin->value() == spin->minimum(), "a preset left an explicit look-ahead");
    require(policy->currentData().toString() == QStringLiteral("reliable"), "an Auto look-ahead no longer matches the preset");
    apply->click();
    QCoreApplication::processEvents();
    require(readBytes(path).count("active_picture_lookahead_frames") == 1, "the preset wrote or kept the key");
}

void testShaderSettingsComeFromTheShaderFile()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("VideoProcessor.cfg");
    require(QDir(dir.path()).mkdir(QStringLiteral("shaders")), "shader folder");
    {
        QFile shader(dir.filePath("shaders/Test dither.glsl"));
        require(shader.open(QIODevice::WriteOnly), "shader fixture");
        shader.write("//!PARAM noise_amplitude\n//!DESC Noise strength\n//!TYPE float\n"
            "//!MINIMUM 0.0\n//!MAXIMUM 0.5\n0.15\n\n"
            "//!PARAM motion\n//!DESC 1 = follow the motion, 0 = plain\n//!TYPE float\n"
            "//!MINIMUM 0.0\n//!MAXIMUM 1.0\n0.0\n\n"
            "//!PARAM strength\n//!DESC Strength\n//!TYPE float\n//!MINIMUM 0.0\n//!MAXIMUM 1.0\n0.5\n\n"
            "//!PARAM bits\n//!DESC Output bits\n//!TYPE DEFINE\n//!MINIMUM 8\n//!MAXIMUM 10\n10\n\n"
            "//!HOOK OUTPUT\n//!BIND HOOKED\nvec4 hook() { return HOOKED_tex(HOOKED_pos); }\n");
        QFile plain(dir.filePath("shaders/Plain.glsl"));
        require(plain.open(QIODevice::WriteOnly), "shader fixture");
        plain.write("//!HOOK OUTPUT\n//!BIND HOOKED\nvec4 hook() { return HOOKED_tex(HOOKED_pos); }\n");
    }
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "fixture");
    file.write("[vprenderer.First]\nquality: high\n"
        "[shader.standard]\ntype: multi\nlabel: Off\n"
        "[shader.standard.dither]\nshader_type: custom\nlabel: Test dither\nglsl_file: Test dither.glsl\n"
        "param_noise_amplitude: 0.3\n"
        "[shader.standard.plain]\nshader_type: custom\nlabel: Plain\nglsl_file: Plain.glsl\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(8);
    auto* shaders = requireControl<QListWidget>(window, QStringLiteral("config.shader.standard.items"));
    auto select = [shaders](const QString& section)
    {
        for (int row = 0; row < shaders->count(); ++row)
            if (shaders->item(row)->data(Qt::UserRole).toString().compare(section, Qt::CaseInsensitive) == 0)
            {
                shaders->setCurrentRow(row);
                QCoreApplication::processEvents();
                return true;
            }
        return false;
    };
    require(select(QStringLiteral("shader.standard.dither")), "dither shader missing");
    auto* noise = requireControl<QDoubleSpinBox>(window, QStringLiteral("config.shader.standard.setting.noise_amplitude"));
    require(std::abs(noise->value() - 0.3) < 1e-9, "The configured value is not shown");
    require(std::abs(noise->maximum() - 0.5) < 1e-9 && noise->minimum() == 0.0, "The shader's range is not used");
    auto* bits = requireControl<QSpinBox>(window, QStringLiteral("config.shader.standard.setting.bits"));
    require(bits->value() == 10, "The shader's default is not shown");
    // An on/off setting (range 0-1, "1 = ..." description) is a tick box;
    // a 0-1 strength stays a number.
    auto* motion = requireControl<QCheckBox>(window, QStringLiteral("config.shader.standard.setting.motion"));
    require(!motion->isChecked(), "The on/off setting does not show its default (off)");
    requireControl<QDoubleSpinBox>(window, QStringLiteral("config.shader.standard.setting.strength"));
    motion->setChecked(true);
    noise->setValue(0.2);
    QCoreApplication::processEvents();
    QPushButton* apply = requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"));
    apply->click();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("param_noise_amplitude: 0.2"), "The shader setting was not saved");
    require(readBytes(path).contains("param_motion: 1"), "The tick box did not save 1");

    require(select(QStringLiteral("shader.standard.plain")), "plain shader missing");
    auto* note = requireControl<QLabel>(window, QStringLiteral("config.shader.standard.settings_note"));
    require(note->text().contains(QStringLiteral("no adjustable settings")),
        "A shader without settings does not say so");
}

void testEveryPageRoundTrips()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    {
        QFile file(path);
        require(file.open(QIODevice::Append),
            "Cannot add the GLSL-only shader fixture");
        const QByteArray glslOnlyMember =
            "\n[shader.nls.plus]\n"
            "shader_type: nls\n"
            "label: NLS+\n"
            "stage: pre_resize\n"
            "glsl_file: NLSPlus.glsl\n"
            "\n[shader.standard]\n"
            "type: multi\n"
            "\n[shader.standard.debanding_mild]\n"
            "shader_type: custom\n"
            "label: Debanding Mild\n"
            "stage: pre_resize\n"
            "hlsl_file: Debanding mild.hlsl\n"
            "threshold: 32\n";
        require(file.write(glslOnlyMember) == glslOnlyMember.size(),
            "Cannot write the GLSL-only shader fixture");
    }
    ConfigEditorWindow window(path, 0, true);

    QStackedWidget* pages = requireControl<QStackedWidget>(window,
        QStringLiteral("settingsPages"));
    require(pages->count() == 25,   // 24 + the VP-V section (ADR-0023)
        "Renderer, Scaling, Color, Output, Screen, Zoom, Processing, shader, shortcut, Subtitles, Dynamic Drive, Updates, Motion and Calibration pages were not added as dedicated settings pages");
    for (QPushButton* button : window.findChildren<QPushButton*>())
        require(!button->property("navChild").toBool(),
            "Grouped settings still expose child entries in the left navigation");
    QTabBar* sectionTabs = requireControl<QTabBar>(window,
        QStringLiteral("settingsSectionTabs"));
    require(window.findChildren<QTabBar*>(
        QStringLiteral("settingsSectionTabs")).size() == 1,
        "Grouped settings do not use one persistent tab control");
    require(!sectionTabs->expanding() &&
        sectionTabs->sizePolicy().horizontalPolicy() == QSizePolicy::Maximum,
        "Grouped settings tabs do not use compact Shader tab sizing");
    require(!sectionTabs->drawBase(),
        "Grouped settings tabs still draw the native white tab-bar base");
    const auto parentButton = [&window](const QString& text)
    {
        for (QPushButton* button : window.findChildren<QPushButton*>())
            if (button->property("nav").toBool() && button->text() == text)
                return button;
        return static_cast<QPushButton*>(nullptr);
    };
    const auto requireTabs = [sectionTabs](const QStringList& expected)
    {
        QStringList actual;
        for (int tab = 0; tab < sectionTabs->count(); ++tab)
            actual.push_back(sectionTabs->tabText(tab));
        require(actual == expected, "A grouped settings page has the wrong top tabs");
    };
    QPushButton* shadersNavigation = parentButton(QStringLiteral("Shaders"));
    QPushButton* shortcutsNavigation = parentButton(QStringLiteral("Shortcuts"));
    QPushButton* vpRenderer = parentButton(QStringLiteral("VP Renderer"));
    QPushButton* directShow = parentButton(QStringLiteral("DirectShow"));
    require(shadersNavigation && shortcutsNavigation && vpRenderer && directShow,
        "A grouped settings parent is missing from the left navigation");
    shadersNavigation->click();
    requireTabs({ QStringLiteral("Setup"), QStringLiteral("Standard"),
        QStringLiteral("NLS") });
    vpRenderer->click();
    requireTabs({ QStringLiteral("Rendering"), QStringLiteral("Color / Output"),
        QStringLiteral("Scaling"),
        QStringLiteral("Screen"), QStringLiteral("Zoom"), QStringLiteral("Subtitles"),
        QStringLiteral("Motion"), QStringLiteral("Processing") });
    directShow->click();
    requireTabs({ QStringLiteral("General"), QStringLiteral("Input Processing") });
    shortcutsNavigation->click();
    requireTabs({ QStringLiteral("Setup"), QStringLiteral("Shortcuts") });
    QWidget* navigation = requireControl<QWidget>(window, QStringLiteral("sidebar"));
    require(navigation->minimumWidth() >= 172,
        "The settings navigation is too narrow for renderer child labels");
    const QString theme = VpTheme::StyleSheet();
    require(theme.contains(QStringLiteral("QPushButton[nav=\"true\"]:focus")) &&
        theme.contains(QStringLiteral("outline: 0")),
        "Focused settings navigation still uses the generic button focus box");
    require(theme.contains(QStringLiteral("QTabBar::tab:selected")) &&
        theme.contains(QStringLiteral("color: #f2f8fd")),
        "Shader section tabs do not have a visible selected state");
    require(theme.contains(QStringLiteral("min-height: 30px")) &&
        theme.contains(QStringLiteral("padding: 3px 12px")),
        "Settings navigation no longer reserves safe vertical label space");
    require(theme.contains(QStringLiteral("QDialog")) &&
        theme.contains(QStringLiteral("QDialog { background: #0b121b; }")),
        "Qt dialogs do not use the Config window background");
    window.show();
    QCoreApplication::processEvents();
    QPushButton* selectedNavigation = nullptr;
    for (QPushButton* button : window.findChildren<QPushButton*>())
    {
        if (!button->property("nav").toBool()) continue;
        require(button->height() >= button->fontMetrics().height() + 6,
            "A settings navigation label can be clipped vertically");
        if (button->isChecked()) selectedNavigation = button;
    }
    require(selectedNavigation != nullptr,
        "Settings navigation has no selected item");
    QImage navigationImage(selectedNavigation->size(), QImage::Format_ARGB32);
    navigationImage.fill(Qt::transparent);
    selectedNavigation->render(&navigationImage);
    require(navigationImage.pixelColor(selectedNavigation->width() - 8,
        selectedNavigation->height() / 2) == QColor(QStringLiteral("#13283a")),
        "Settings navigation stylesheet did not render its selected background");

    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.general.capture_device")),
        QStringLiteral("Decklink Test Device"));
    QComboBox* captureInput = requireControl<QComboBox>(window,
        QStringLiteral("config.general.capture_input"));
    selectData(captureInput, QStringLiteral("HDMI"));
    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.general.renderer")),
        QStringLiteral("VideoProcessor Renderer (Alpha)"));
    requireControl<QCheckBox>(window, QStringLiteral("config.general.fullscreen"))->setChecked(false);
    requireControl<QCheckBox>(window, QStringLiteral("config.general.startminimized"))->setChecked(false);
    // Delta 6: saved start state for what keys / window controls change.
    require(!requireControl<QCheckBox>(window, QStringLiteral("config.general.stats_overlay"))->isChecked(),
        "The statistics overlay did not default to hidden");
    require(requireControl<QCheckBox>(window, QStringLiteral("config.general.use_frame_queue"))->isChecked() &&
        requireControl<QCheckBox>(window, QStringLiteral("config.general.queue_auto_reset"))->isChecked(),
        "The queue checks did not default to on");
    require(!requireControl<QCheckBox>(window, QStringLiteral("config.general.persist_profile_selection"))->isChecked(),
        "Key-selected profiles did not default to session-only");
    requireControl<QCheckBox>(window, QStringLiteral("config.general.scene_detect"))->setChecked(false);
    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.general.fullscreen_monitor_name")),
        QStringLiteral("Test Display"));
    selectData(requireControl<QComboBox>(window, QStringLiteral("config.general.video_conversion")),
        QStringLiteral("V210_TO_P010"));
    selectData(requireControl<QComboBox>(window, QStringLiteral("config.general.container_colorspace")),
        QStringLiteral("BT2020"));
	QSpinBox* profileChangeDisplay = requireControl<QSpinBox>(window,
		QStringLiteral("config.general.profile_change_display_seconds"));
	require(profileChangeDisplay->value() == 5,
		"Profile change display did not default to five seconds");
	profileChangeDisplay->setValue(0);
	require(profileChangeDisplay->specialValueText() == QStringLiteral("Off"),
		"Zero profile change display duration is not labelled Off");

    requireControl<QSpinBox>(window, QStringLiteral("config.queue.queue_size"))->setValue(48);
    requireControl<QSpinBox>(window, QStringLiteral("config.queue.lead_frames"))->setValue(3);
    requireControl<QSpinBox>(window,
        QStringLiteral("config.queue.reset_queue_too_large_percent"))->setValue(200);
    requireControl<QLineEdit>(window, QStringLiteral("config.queue.shortcut"))
        ->setText(QStringLiteral("Ctrl+q"));

    selectData(requireControl<QComboBox>(window, QStringLiteral("config.vprenderer.quality")),
        QStringLiteral("balanced"));
    requireControl<QLineEdit>(window, QStringLiteral("config.vprenderer.sdr_target_nits"))
        ->setText(QStringLiteral("220"));
    selectData(requireControl<QComboBox>(window, QStringLiteral("config.vprenderer.tone_mapping")),
        QStringLiteral("spline"));
    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.input_processing.video_conversion")),
        QStringLiteral("V210_TO_P010"));
    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.input_processing.hdr_colorspace")),
        QStringLiteral("FOLLOW_INPUT_LLDV"));

    requireControl<QLineEdit>(window,
        QStringLiteral("config.vprenderer.viewport.screen_aspect"))->setText(QStringLiteral("21:10"));
    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.viewport.vertical_alignment")),
        QStringLiteral("bottom"));
    requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.zoom.automatic_crop"))->setCheckState(Qt::Checked);
    requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.zoom.crop_narrower_content_to_fill_screen"))->setCheckState(Qt::Checked);
    requireControl<QLineEdit>(window,
        QStringLiteral("config.vprenderer.zoom.crop_narrower_content_aspect_limit"))
        ->setText(QStringLiteral("2.20:1"));
    requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.zoom.crop_wider_content_to_fill_screen"))->setCheckState(Qt::Checked);
    requireControl<QLineEdit>(window,
        QStringLiteral("config.vprenderer.zoom.crop_wider_content_aspect_limit"))
        ->setText(QStringLiteral("2.76:1"));
    requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.viewport.anamorphic_enabled"))->setChecked(true);
    requireControl<QLineEdit>(window,
        QStringLiteral("config.vprenderer.viewport.anamorphic_scale"))->setText(QStringLiteral("4:3"));

    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.directshow.renderer_start_stop_time_method")),
        QStringLiteral("RATIONAL_RATIONAL"));
    requireControl<QCheckBox>(window,
        QStringLiteral("config.directshow.frame_offset.auto"))->setChecked(false);
    requireControl<QSpinBox>(window,
        QStringLiteral("config.directshow.frame_offset.value"))->setValue(75);
    QSpinBox* frameOffsetValue = requireControl<QSpinBox>(window,
        QStringLiteral("config.directshow.frame_offset.value"));
    QCheckBox* frameOffsetAuto = requireControl<QCheckBox>(window,
        QStringLiteral("config.directshow.frame_offset.auto"));
    require(frameOffsetValue->parentWidget() == frameOffsetAuto->parentWidget() &&
        frameOffsetValue->parentWidget()->layout()->indexOf(frameOffsetValue) <
        frameOffsetValue->parentWidget()->layout()->indexOf(frameOffsetAuto),
        "Frame offset Auto is not positioned after the numeric entry");
    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.directshow.renderer_primaries")), QStringLiteral("BT2020"));
    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.directshow.video_conversion")), QStringLiteral("NONE"));
    selectData(requireControl<QComboBox>(window,
        QStringLiteral("config.directshow.container_colorspace")), QStringLiteral("REC709"));

    requireControl<QLineEdit>(window, QStringLiteral("config.lldv.max_cll"))
        ->setText(QStringLiteral("1200"));
    requireControl<QLineEdit>(window, QStringLiteral("config.lldv.max_fall"))
        ->setText(QStringLiteral("450"));

    requireControl<QLineEdit>(window, QStringLiteral("config.shortcuts.fullscreen_toggle"))
        ->setText(QStringLiteral("Ctrl+f"));
    QLineEdit* configShortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.config_editor"));
    require(configShortcut->text() == QStringLiteral("Ctrl+Shift+S"),
        "Open configuration did not default to Ctrl+Shift+S");
    configShortcut->setText(QStringLiteral("Ctrl+e"));
    QLineEdit* renderedCaptureShortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.capture_rendered_output"));
    require(renderedCaptureShortcut->text() == QStringLiteral("Ctrl+Alt+S"),
        "Rendered-output capture did not default to Ctrl+Alt+S");
    renderedCaptureShortcut->setText(QStringLiteral("Ctrl+Alt+C"));
    QLineEdit* stateReportShortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.state_report"));
    require(stateReportShortcut->text() == QStringLiteral("Ctrl+Alt+D"),
        "Save problem report did not default to Ctrl+Alt+D");
    stateReportShortcut->setText(QStringLiteral("Ctrl+Alt+Q"));
    QLineEdit* reapplyRulesShortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.reapply_rules"));
    require(reapplyRulesShortcut->text().isEmpty(),
        "Re-apply rules unexpectedly received a built-in shortcut");
    reapplyRulesShortcut->setText(QStringLiteral("Ctrl+Alt+R"));
    QLineEdit* showProfilesShortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.show_profiles"));
    require(showProfilesShortcut->text() == QStringLiteral("Ctrl+Alt+I"),
        "Show profiles did not default to Ctrl+Alt+I");
    showProfilesShortcut->setText(QStringLiteral("Ctrl+Alt+P"));
    QLineEdit* noUiShortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.toggle_noui"));
    require(noUiShortcut->text() == QStringLiteral("Ctrl+Shift+U"),
        "Video-only UI toggle did not default to Ctrl+Shift+U");
    noUiShortcut->setText(QStringLiteral("Alt+u"));
    QCheckBox* foregroundOnly = requireControl<QCheckBox>(window,
        QStringLiteral("config.shortcuts.foreground_only"));
    require(!foregroundOnly->isChecked(),
        "Foreground-only shortcut processing did not default to disabled");
    foregroundOnly->setChecked(true);
    // Delta 6: bare keys stay local by default; DirectShow-only keys are marked.
    require(!requireControl<QCheckBox>(window, QStringLiteral("config.shortcuts.global_hotkeys"))->isChecked(),
        "Global single-key shortcuts did not default to off");

    window.selectPage(10);
    require(requireControl<QCheckBox>(window, QStringLiteral("config.logging.enabled"))->isChecked(),
        "Logging did not default to enabled");
    require(!requireControl<QCheckBox>(window, QStringLiteral("config.logging.debug"))->isChecked(),
        "Enhanced logging did not default to disabled");
    require(requireControl<QSpinBox>(window,
        QStringLiteral("config.logging.debug_log_retention"))->value() == 10,
        "Debug log retention did not default to 10 files");
    requireControl<QSpinBox>(window,
        QStringLiteral("config.logging.debug_log_retention"))->setValue(25);
    requireControl<QCheckBox>(window, QStringLiteral("config.logging.debug"))->setChecked(true);
    require(!requireControl<QSpinBox>(window,
        QStringLiteral("config.logging.debug_log_retention"))->isEnabled(),
        "Log retention remained editable while enhanced logging was enabled");
    QCheckBox* autoReport = requireControl<QCheckBox>(window,
        QStringLiteral("config.logging.auto_report"));
    require(!autoReport->isChecked(), "Automatic problem reports did not default to off");
    autoReport->setChecked(true);
    QCheckBox* reportThumbnail = requireControl<QCheckBox>(window,
        QStringLiteral("config.logging.report_thumbnail"));
    require(!reportThumbnail->isChecked(), "Problem-report picture did not default to off");
    reportThumbnail->setChecked(true);
    requireControl<QCheckBox>(window, QStringLiteral("config.logging.debug"))->setChecked(false);
    requireControl<QCheckBox>(window, QStringLiteral("config.logging.enabled"))->setChecked(false);
    require(!requireControl<QSpinBox>(window,
        QStringLiteral("config.logging.debug_log_retention"))->isEnabled(),
        "Debug log retention remained editable while logging was disabled");

    QListWidget* actions = requireControl<QListWidget>(window,
        QStringLiteral("config.actions.items"));
    require(actions->count() > 0, "Actions fixture did not load");
    actions->setCurrentRow(0);
    requireControl<QLineEdit>(window, QStringLiteral("config.actions.run"))
        ->setText(QStringLiteral("C:\\Tools\\verified-action.cmd 42"));
    selectData(requireControl<QComboBox>(window, QStringLiteral("config.actions.renderer")),
        QStringLiteral("*"));

    window.selectPage(9);
    QListWidget* shaders = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.nls.modes"));
    require(shaders->count() > 1, "NLS fixture did not load");
    require(!shaders->item(0)->text().contains(QStringLiteral("(Default)")),
        "NLS still displays a hard-coded Default role beside the profile name");
    shaders->setCurrentRow(0);
    requireControl<QLineEdit>(window, QStringLiteral("config.shader.nls.label"))
        ->setText(QStringLiteral("Off"));
    require(shaders->currentItem()->text() == QStringLiteral("Off"),
        "A root NLS profile named Off was replaced by hard-coded Default text");
    require(shaders->dragDropMode() == QAbstractItemView::InternalMove,
        "NLS profiles do not support drag reordering");
    QPushButton* addNls = requireControl<QPushButton>(window,
        QStringLiteral("config.shader.nls.add"));
    QPushButton* removeNls = requireControl<QPushButton>(window,
        QStringLiteral("config.shader.nls.remove"));
    shaders->setCurrentRow(0);
    require(removeNls->isEnabled(),
        "The first NLS profile is still protected as a fixed Default row");
    const int nlsCountBeforeAdd = shaders->count();
    addNls->click();
    require(shaders->count() == nlsCountBeforeAdd + 1 && shaders->currentItem(),
        "Add NLS profile did not create and select a named profile");
    requireControl<QLineEdit>(window, QStringLiteral("config.shader.nls.label"))
        ->setText(QStringLiteral("Temporary NLS"));
    require(shaders->currentItem()->text() == QStringLiteral("Temporary NLS"),
        "NLS display-name changes did not update the shared list immediately");
    answerMessageBox(QMessageBox::Yes);
    removeNls->click();
    require(shaders->count() == nlsCountBeforeAdd,
        "Remove NLS profile did not remove the selected named profile");
    shaders->setCurrentRow(1);
    requireControl<QLineEdit>(window, QStringLiteral("config.shader.nls.label"))
        ->setText(QStringLiteral("Verified Stretch"));
    require(window.findChild<QSpinBox*>(QStringLiteral("config.shader.nls.order")) == nullptr,
        "NLS mode order remained exposed as a numeric setting");
    require(window.findChild<QCheckBox*>(QStringLiteral("config.shader.nls.show_parameters")) == nullptr,
        "Custom shader parameters remained a checkbox setting");
    QToolButton* parameterToggle = requireControl<QToolButton>(window,
        QStringLiteral("config.shader.nls.parameters_toggle"));
    require(!parameterToggle->isChecked(), "Custom shader parameters did not start collapsed");
    parameterToggle->click();
    require(parameterToggle->isChecked(), "Custom shader parameter disclosure did not expand");
    QTableWidget* parameters = requireControl<QTableWidget>(window,
        QStringLiteral("config.shader.nls.parameters"));
    require(theme.contains(QStringLiteral("QTableWidget::item:selected")) &&
        theme.contains(QStringLiteral("background: #173c59")),
        "Selected shader parameter rows can fall back to the platform light selection color");
    require(parameters->rowCount() > 0, "Shader parameter fixture did not load");
    const QModelIndex parameterIndex = parameters->model()->index(0, 1);
    parameters->edit(parameterIndex);
    QCoreApplication::processEvents();
    QLineEdit* parameterEditor = parameters->findChild<QLineEdit*>(
        QStringLiteral("config.shader.nls.parameter_value_editor"));
    require(parameterEditor != nullptr, "Shader parameter did not use the compact editor");
    const QRect valueCell = parameters->visualRect(parameterIndex);
    const QRect expectedEditor = valueCell.adjusted(1, 1, -1, -1);
    require(parameterEditor->geometry() == expectedEditor,
        "Shader parameter editor was not aligned to its value cell");
    bool changedParameter = false;
    for (int row = 0; row < parameters->rowCount(); ++row)
        if (parameters->item(row, 0) &&
            parameters->item(row, 0)->text().compare(QStringLiteral("strength"),
                Qt::CaseInsensitive) == 0)
        {
            parameters->item(row, 1)->setText(QStringLiteral("0.85"));
            changedParameter = true;
            break;
        }
    require(changedParameter, "Structured shader parameter did not load");
    const int nlsDragSource = shaders->currentRow();
    require(shaders->model()->moveRow(QModelIndex(), nlsDragSource,
        QModelIndex(), nlsDragSource + 2),
        "The shared NLS list model rejected a drag reorder");

    int nlsPlusRow = -1;
    for (int row = 0; row < shaders->count(); ++row)
        if (shaders->item(row)->text() == QStringLiteral("NLS+"))
        {
            nlsPlusRow = row;
            break;
        }
    require(nlsPlusRow >= 0, "NLS+ shader mode did not load");
    shaders->setCurrentRow(nlsPlusRow);
    require(requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.nls.hlsl_file"))->text().isEmpty(),
        "A GLSL-only shader was given an unintended DirectShow fallback file");
    require(requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.nls.glsl_file"))->text() == QStringLiteral("NLSPlus.glsl"),
        "NLS+ did not retain its VP Renderer implementation");
    shaders->setCurrentRow(0);
    answerMessageBox(QMessageBox::Yes);
    removeNls->click();
    require(shaders->count() == nlsCountBeforeAdd - 1,
        "The first NLS profile could not be removed through the shared lifecycle");

    window.selectPage(14);
    QTabBar* shaderSections = requireControl<QTabBar>(window,
        QStringLiteral("settingsSectionTabs"));
    require(shaderSections->count() == 3 &&
        shaderSections->tabText(0) == QStringLiteral("Setup") &&
        shaderSections->tabText(1) == QStringLiteral("Standard") &&
        shaderSections->tabText(2) == QStringLiteral("NLS") &&
        shaderSections->currentIndex() == 0,
        "Shaders page does not separate Setup, Standard, and NLS in the requested order");
    require(window.findChild<QPushButton*>(
        QStringLiteral("config.shader.prepare")) == nullptr,
        "Shaders Setup still exposes exhaustive preparation");
    const QString cacheDirectoryPath = directory.filePath(QStringLiteral("vprenderer"));
    require(QDir().mkpath(cacheDirectoryPath),
        "Cannot create the shader-cache test directory");
    const QString cachePath = QDir(cacheDirectoryPath).filePath(
        QStringLiteral("VideoProcessorShaderCache.bin"));
    QFile cacheFile(cachePath);
    require(cacheFile.open(QIODevice::WriteOnly) && cacheFile.write("cached") == 6,
        "Cannot create the shader-cache test fixture");
    cacheFile.close();
    requireControl<QPushButton>(window,
        QStringLiteral("config.shader.cache.clear"))->click();
    require(!QFileInfo::exists(cachePath),
        "Clear shader cache did not remove the persistent cache file");
    require(QFileInfo::exists(QDir(cacheDirectoryPath).filePath(
        QStringLiteral("VideoProcessorShaderCache.clear"))),
        "Clear shader cache did not leave a request for the active renderer shutdown path");
    QListWidget* standardShaders = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.standard.items"));
    require(standardShaders->count() == 2,
        "Standard shader fixture did not load");
    require(standardShaders->dragDropMode() == QAbstractItemView::InternalMove,
        "Standard profiles do not support drag reordering");
    require(!standardShaders->item(0)->text().contains(
        QStringLiteral("(Default)")),
        "Standard shaders still display a hard-coded Default role");
    standardShaders->setCurrentRow(0);
    requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.standard.label"))->setText(
            QStringLiteral("Off"));
    require(standardShaders->currentItem()->text() == QStringLiteral("Off"),
        "A root Standard profile named Off was replaced by hard-coded Default text");

    const auto hasExactHelp = [&window](const QString& sentence)
    {
        const QList<QLabel*> labels = window.findChildren<QLabel*>();
        return std::any_of(labels.begin(), labels.end(), [&sentence](QLabel* label)
            { return label->text() == sentence; });
    };
    require(hasExactHelp(QStringLiteral(
        "A single NLS profile can be active.")),
        "The required NLS active-profile explanation is missing");
    require(hasExactHelp(QStringLiteral(
        "Multiple standard shaders profile can be active.")),
        "The required Standard active-profile explanation is missing");
    int debandingRow = -1;
    for (int row = 0; row < standardShaders->count(); ++row)
        if (standardShaders->item(row)->text() == QStringLiteral("Debanding Mild"))
        {
            debandingRow = row;
            break;
        }
    require(debandingRow >= 0, "Debanding default shader did not load");
    standardShaders->setCurrentRow(debandingRow);
    require(requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.standard.shortcut"))->text().isEmpty(),
        "A shipped standard shader was assigned a shortcut by default");
    require(requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.standard.hlsl_file"))->text() ==
        QStringLiteral("Debanding mild.hlsl"),
        "Standard DirectShow shader file did not load");
    require(requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.standard.glsl_file"))->text().isEmpty(),
        "HLSL-only standard shader was given a VP Renderer fallback file");
    QToolButton* standardParameterToggle = requireControl<QToolButton>(window,
        QStringLiteral("config.shader.standard.parameters_toggle"));
    require(!standardParameterToggle->isChecked(),
        "Standard shader custom parameters did not start collapsed");
    standardParameterToggle->click();
    QTableWidget* standardParameters = requireControl<QTableWidget>(window,
        QStringLiteral("config.shader.standard.parameters"));
    require(standardParameters->rowCount() == 1 &&
        standardParameters->item(0, 0)->text() == QStringLiteral("threshold") &&
        standardParameters->item(0, 1)->text() == QStringLiteral("32"),
        "Standard shader custom parameters did not load");
    standardParameters->item(0, 1)->setText(QStringLiteral("28"));
    requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.standard.shortcut"))->setText(QStringLiteral("Ctrl+d"));
    const int standardCountBeforeAdd = standardShaders->count();
    requireControl<QPushButton>(window, QStringLiteral("config.shader.standard.add"))->click();
    require(standardShaders->count() == standardCountBeforeAdd + 1,
        "Add standard shader did not create a shader configuration");
    require(standardShaders->currentItem(),
        "New standard shader was not selected");
    requireControl<QLineEdit>(window, QStringLiteral("config.shader.standard.label"))
        ->setText(QStringLiteral("Temporary Shader"));
    require(standardShaders->currentItem()->text() == QStringLiteral("Temporary Shader"),
        "Standard display-name changes did not update the shared list immediately");
    require(requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.standard.shortcut"))->text().isEmpty(),
        "New standard shader unexpectedly received a shortcut");
    require(requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.standard.hlsl_file"))->text().isEmpty() &&
        requireControl<QLineEdit>(window,
            QStringLiteral("config.shader.standard.glsl_file"))->text().isEmpty(),
        "New standard shader did not begin with intentionally blank backend files");
    requireControl<QPushButton>(window,
        QStringLiteral("config.shader.standard.move_up"))->click();
    require(standardShaders->currentRow() == 1 &&
        standardShaders->currentItem()->text() == QStringLiteral("Temporary Shader"),
        "Move up did not reorder the selected standard shader profile");
    requireControl<QPushButton>(window, QStringLiteral("config.shader.standard.add"))->click();
    requireControl<QLineEdit>(window, QStringLiteral("config.shader.standard.label"))
        ->setText(QStringLiteral("Transient Shader"));
    require(standardShaders->currentItem()->text() == QStringLiteral("Transient Shader"),
        "Second standard shader profile was not created");
    answerMessageBox(QMessageBox::Yes);
    requireControl<QPushButton>(window, QStringLiteral("config.shader.standard.remove"))->click();
    require(standardShaders->count() == standardCountBeforeAdd + 1,
        "Remove standard shader did not remove the selected named profile");
    standardShaders->setCurrentRow(0);
    answerMessageBox(QMessageBox::Yes);
    requireControl<QPushButton>(window,
        QStringLiteral("config.shader.standard.remove"))->click();
    require(standardShaders->count() == standardCountBeforeAdd,
        "The first Standard profile could not be removed through the shared lifecycle");

    save(window);
    const QByteArray saved = readBytes(path);
    const QList<QByteArray> expected = {
        "capture_device: Decklink Test Device", "capture_input: HDMI",
        "renderer: VP Renderer", "fullscreen: false",
		"profile_change_display_seconds: 0",
        "scene_detect: false",
        "fullscreen_monitor_name: Test Display", "container_colorspace: BT2020",
        "queue_size: 48", "lead_frames: 3", "reset_queue_too_large_percent: 200",
        "shortcut: Ctrl+Q", "quality: balanced", "sdr_target_nits: 220", "tone_mapping: spline",
        "screen_aspect: 21:10", "vertical_alignment: bottom", "anamorphic_scale: 4:3",
        "automatic_crop: true",
        "crop_narrower_content_to_fill_screen: true",
        "crop_narrower_content_aspect_limit: 2.20:1",
        "crop_wider_content_to_fill_screen: true",
        "crop_wider_content_aspect_limit: 2.76:1",
        "renderer_start_stop_time_method: RATIONAL_RATIONAL", "frame_offset: 75",
        "renderer_primaries: BT2020", "video_conversion: NONE",
        "container_colorspace: REC709", "max_cll: 1200", "max_fall: 450",
        "fullscreen_toggle: Ctrl+F", "config_editor: Ctrl+E",
        "capture_rendered_output: Ctrl+Alt+C", "toggle_noui: Alt+U",
        "state_report: Ctrl+Alt+Q", "auto_report: true", "report_thumbnail: true",
        "reapply_rules: Ctrl+Alt+R",
        "foreground_only: true",
        "renderer: *", "run: C:\\Tools\\verified-action.cmd 42",
        "enabled: false", "debug: false", "debug_log_retention: 25",
        "label: Verified Stretch", "order: 10", "strength: 0.85", "threshold: 28",
        "shortcut: Ctrl+D", "[shader.standard.New_1]",
        "label: Temporary Shader"
    };
    for (const QByteArray& text : expected)
        require(saved.contains(text), text.constData());
    const int rendererRoot = saved.indexOf("[vprenderer.input_processing]");
    const int rendererRootEnd = saved.indexOf(QByteArray("\n["),
        rendererRoot + 1);
    const QByteArray rendererRootValues = rendererRoot >= 0 ? saved.mid(
        rendererRoot, rendererRootEnd < 0 ? -1 : rendererRootEnd - rendererRoot) :
        QByteArray();
    require(rendererRootValues.contains("video_conversion: V210_TO_P010"),
        "VP Renderer input override was not saved in its independent input-policy section");
    require(saved.indexOf("[shader.nls.protected]") < saved.indexOf("[shader.nls.standard]"),
        "NLS selection order was not persisted through section order");
    require(saved.indexOf("[shader.standard.New_1]") <
        saved.indexOf("[shader.standard.debanding_mild]"),
        "Standard profile execution order was not persisted through section order");

    ConfigEditorWindow reloaded(path, 0, true);
    require(!requireControl<QCheckBox>(reloaded,
        QStringLiteral("config.general.fullscreen"))->isChecked(),
        "General Boolean did not reload");
	require(requireControl<QSpinBox>(reloaded,
		QStringLiteral("config.general.profile_change_display_seconds"))->value() == 0,
		"Disabled profile change display duration did not reload");
    require(requireControl<QSpinBox>(reloaded,
        QStringLiteral("config.queue.queue_size"))->value() == 48,
        "Queue value did not reload");
    require(requireControl<QLineEdit>(reloaded,
        QStringLiteral("config.vprenderer.viewport.screen_aspect"))->text() ==
        QStringLiteral("21:10"), "Viewport value did not reload");
    require(requireControl<QCheckBox>(reloaded,
        QStringLiteral("config.vprenderer.zoom.automatic_crop"))->isChecked(),
        "Automatic black-bar crop did not reload in the Zoom profile");
    require(requireControl<QLineEdit>(reloaded,
        QStringLiteral("config.vprenderer.zoom.crop_narrower_content_aspect_limit"))->text() ==
        QStringLiteral("2.20:1"), "Narrower-content aspect limit did not reload");
    require(requireControl<QLineEdit>(reloaded,
        QStringLiteral("config.vprenderer.zoom.crop_wider_content_aspect_limit"))->text() ==
        QStringLiteral("2.76:1"), "Wider-content aspect limit did not reload");
    require(requireControl<QComboBox>(reloaded,
        QStringLiteral("config.vprenderer.viewport.vertical_alignment"))->currentData().toString() ==
        QStringLiteral("bottom"), "Vertical alignment did not reload");
    require(requireControl<QComboBox>(reloaded,
        QStringLiteral("config.vprenderer.input_processing.video_conversion"))->currentData().toString() ==
        QStringLiteral("V210_TO_P010"), "VP Renderer input override did not reload");
    require(requireControl<QComboBox>(reloaded,
        QStringLiteral("config.directshow.video_conversion"))->currentData().toString() ==
        QStringLiteral("NONE"), "DirectShow input override did not reload");
    require(requireControl<QCheckBox>(reloaded,
        QStringLiteral("config.shortcuts.foreground_only"))->isChecked(),
        "Foreground-only shortcut processing did not reload");
    require(requireControl<QLineEdit>(reloaded,
        QStringLiteral("config.shortcuts.reapply_rules"))->text() ==
        QStringLiteral("Ctrl+Alt+R"), "Re-apply rules shortcut did not reload");
    require(requireControl<QLineEdit>(reloaded,
        QStringLiteral("config.shortcuts.show_profiles"))->text() ==
        QStringLiteral("Ctrl+Alt+P"), "Show profiles shortcut did not reload");
}

void testRendererSectionTabsRemainSynchronizedDuringRapidClicks()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true);
    window.resize(1120, 760);
    window.show();
    window.selectPage(2);
    QCoreApplication::processEvents();

    QStackedWidget* pages = requireControl<QStackedWidget>(window,
        QStringLiteral("settingsPages"));
    QTabBar* sectionTabs = requireControl<QTabBar>(window,
        QStringLiteral("settingsSectionTabs"));
    require(window.findChildren<QTabBar*>(
        QStringLiteral("settingsSectionTabs")).size() == 1,
        "Grouped settings tabs are not one persistent control");

    const auto clickTab = [](QTabBar* tabs, int tabIndex)
    {
        const QPoint point = tabs->tabRect(tabIndex).center();
        const QPoint globalPoint = tabs->mapToGlobal(point);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(point),
            QPointF(globalPoint), Qt::LeftButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(tabs, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
            QPointF(globalPoint), Qt::LeftButton, Qt::NoButton,
            Qt::NoModifier);
        QApplication::sendEvent(tabs, &release);
    };
    const auto requireTabs = [sectionTabs](const QStringList& expected)
    {
        QStringList actual;
        for (int index = 0; index < sectionTabs->count(); ++index)
            actual.push_back(sectionTabs->tabText(index));
        require(actual == expected,
            "Persistent grouped tab labels changed unexpectedly");
    };
    const auto requirePage = [&window, pages](QTabBar* tabs, int tabIndex,
        int pageIndex, const QString& title, const QString& sentinelName)
    {
        require(tabs->currentIndex() == tabIndex,
            "Grouped tab selection is stale");
        require(pages->currentIndex() == pageIndex,
            "Grouped page content did not follow its selected tab");
        QLabel* pageTitle = pages->currentWidget()->findChild<QLabel*>(
            QStringLiteral("pageTitle"));
        require(pageTitle && pageTitle->text() == title,
            "Grouped page title did not follow its selected tab");
        QWidget* sentinel = requireControl<QWidget>(*pages->currentWidget(), sentinelName);
        if (!pages->currentWidget()->isAncestorOf(sentinel) ||
            !sentinel->isVisibleTo(&window))
            throw std::runtime_error(QStringLiteral(
                "Grouped page displayed content does not match tab %1 (%2): descendant=%3 visible=%4")
                .arg(tabIndex).arg(title)
                .arg(pages->currentWidget()->isAncestorOf(sentinel))
                .arg(sentinel->isVisibleTo(&window)).toStdString());
    };
    const auto navigationButton = [&window](const QString& text)
    {
        for (QPushButton* button : window.findChildren<QPushButton*>())
            if (button->property("nav").toBool() && button->text() == text)
                return button;
        return static_cast<QPushButton*>(nullptr);
    };
    struct ExpectedPage
    {
        int tabIndex;
        int pageIndex;
        const char* title;
        const char* sentinel;
    };
    const auto runSequence = [sectionTabs, clickTab, requirePage](
        const std::vector<ExpectedPage>& sequence)
    {
        for (const ExpectedPage& expected : sequence)
        {
            clickTab(sectionTabs, expected.tabIndex);
            requirePage(sectionTabs, expected.tabIndex, expected.pageIndex,
                QString::fromLatin1(expected.title),
                QString::fromLatin1(expected.sentinel));
        }
    };

    requireTabs({ QStringLiteral("Rendering"), QStringLiteral("Color / Output"),
        QStringLiteral("Scaling"),
        QStringLiteral("Screen"), QStringLiteral("Zoom"), QStringLiteral("Subtitles"),
        QStringLiteral("Motion"), QStringLiteral("Processing") });
    runSequence({
        { 1, 16, "Color / Output", "config.vprenderer.color.profiles" },
        { 3, 4, "Screen", "config.vprenderer.viewport.profiles" },
        { 4, 18, "Zoom", "config.vprenderer.zoom.crop_narrower_content_to_fill_screen" },
        { 5, 19, "Subtitles", "config.vprenderer.subtitles.profiles" },
        { 6, 22, "Motion", "config.vprenderer.motion.profiles" },   // page 22 after Dynamic Drive (20) and Updates (21)
        { 7, 11, "Input processing", "config.vprenderer.input_processing.video_conversion" },
        { 1, 16, "Color / Output", "config.vprenderer.color.profiles" },
        { 2, 17, "Scaling", "config.vprenderer.scaling.profiles" },
        { 0, 2, "Rendering", "config.vprenderer.profiles" },
        { 5, 19, "Subtitles", "config.vprenderer.subtitles.profiles" },
        { 6, 22, "Motion", "config.vprenderer.motion.profiles" },   // page 22 after Dynamic Drive (20) and Updates (21)
        { 7, 11, "Input processing", "config.vprenderer.input_processing.video_conversion" },
        { 4, 18, "Zoom", "config.vprenderer.zoom.crop_narrower_content_to_fill_screen" },
        { 3, 4, "Screen", "config.vprenderer.viewport.profiles" },
        { 1, 16, "Color / Output", "config.vprenderer.color.profiles" },
        { 2, 17, "Scaling", "config.vprenderer.scaling.profiles" },
        { 0, 2, "Rendering", "config.vprenderer.profiles" }
    });
    require(navigationButton(QStringLiteral("VP Renderer")) &&
        navigationButton(QStringLiteral("VP Renderer"))->isChecked(),
        "VP Renderer child page lost its parent navigation selection");
    window.selectPage(4);
    QListWidget* screenProfiles = requireControl<QListWidget>(*pages->widget(4),
        QStringLiteral("config.vprenderer.viewport.profiles"));
    if (screenProfiles->count() > 1)
        screenProfiles->setCurrentRow(1);
    const QString selectedScreenProfile = screenProfiles->currentItem() ?
        screenProfiles->currentItem()->data(Qt::UserRole).toString() : QString();
    window.selectPage(18);
    QListWidget* zoomProfiles = requireControl<QListWidget>(*pages->widget(18),
        QStringLiteral("config.vprenderer.zoom.profiles"));
    require(zoomProfiles->currentItem(), "Zoom profiles did not initialize");
    if (zoomProfiles->count() > 1)
        zoomProfiles->setCurrentRow(zoomProfiles->count() - 1);
    window.selectPage(4);
    require(screenProfiles->currentItem() &&
        screenProfiles->currentItem()->data(Qt::UserRole).toString() == selectedScreenProfile,
        "Selecting a Zoom profile changed the selected Screen profile");

    window.selectPage(3);
    requireTabs({ QStringLiteral("General"), QStringLiteral("Input Processing") });
    runSequence({
        { 1, 12, "Input processing", "config.directshow.video_conversion" },
        { 0, 3, "General", "config.directshow.frame_offset.auto" },
        { 1, 12, "Input processing", "config.directshow.video_conversion" },
        { 0, 3, "General", "config.directshow.frame_offset.auto" }
    });
    require(navigationButton(QStringLiteral("DirectShow")) &&
        navigationButton(QStringLiteral("DirectShow"))->isChecked(),
        "DirectShow child page lost its parent navigation selection");

    window.selectPage(8);
    requireTabs({ QStringLiteral("Setup"), QStringLiteral("Standard"),
        QStringLiteral("NLS") });
    runSequence({
        { 2, 9, "NLS", "config.shader.nls.modes" },
        { 1, 8, "Standard", "config.shader.standard.items" },
        { 0, 14, "Shaders", "config.shader.cache.clear" },
        { 2, 9, "NLS", "config.shader.nls.modes" },
        { 1, 8, "Standard", "config.shader.standard.items" }
    });
    require(navigationButton(QStringLiteral("Shaders")) &&
        navigationButton(QStringLiteral("Shaders"))->isChecked(),
        "Shader child page lost its parent navigation selection");

    window.selectPage(15);
    requireTabs({ QStringLiteral("Setup"), QStringLiteral("Shortcuts") });
    runSequence({
        { 1, 6, "Shortcuts", "config.shortcuts.fullscreen_toggle" },
        { 0, 15, "Shortcuts", "config.shortcuts.foreground_only" },
        { 1, 6, "Shortcuts", "config.shortcuts.fullscreen_toggle" }
    });
    require(navigationButton(QStringLiteral("Shortcuts")) &&
        navigationButton(QStringLiteral("Shortcuts"))->isChecked(),
        "Shortcuts child page lost its parent navigation selection");
    require(!sectionTabs->drawBase(),
        "Persistent grouped tabs restored the native white tab-bar base");
}

void testEmptyStandardShadersCanCreateFirstShader()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(8);
    QTabBar* shaderSections = requireControl<QTabBar>(window,
        QStringLiteral("settingsSectionTabs"));
    require(shaderSections->tabText(0) == QStringLiteral("Setup") &&
        shaderSections->tabText(1) == QStringLiteral("Standard") &&
        shaderSections->currentIndex() == 1,
        "Standard shader editing is not the selected shader section");
    window.show();
    QCoreApplication::processEvents();
    QListWidget* standardShaders = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.standard.items"));
    require(standardShaders->count() == 0,
        "The editor synthesized a Standard profile that is not configured");

    requireControl<QPushButton>(window, QStringLiteral("config.shader.standard.add"))->click();
    require(standardShaders->count() == 1 && standardShaders->currentItem(),
        "The first Standard shader was not created and selected");
    requireControl<QLineEdit>(window, QStringLiteral("config.shader.standard.label"))
        ->setText(QStringLiteral("My First Shader"));
    require(standardShaders->currentItem()->text().startsWith(
        QStringLiteral("My First Shader")),
        "The first Standard shader name did not update immediately");
    require(requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.standard.shortcut"))->text().isEmpty(),
        "The first Standard shader unexpectedly received a shortcut");

    save(window);
    const QByteArray saved = readBytes(path);
    require(!saved.contains("[shader.standard]\ntype: multi"),
        "Creating a profile recreated the obsolete fixed Standard root");
    const int sectionStart = saved.indexOf("[shader.standard.New_1]");
    const int sectionEnd = saved.indexOf(QByteArray("\n["), sectionStart + 1);
    const QByteArray created = sectionStart >= 0 ? saved.mid(sectionStart,
        sectionEnd < 0 ? -1 : sectionEnd - sectionStart) : QByteArray();
    require(created.contains("shader_type: custom") &&
        created.contains("label: My First Shader") &&
        !created.contains("shortcut:") && !created.contains("when:"),
        "The first Standard shader did not save as a clean opt-in custom shader");
}

void testTwoColumnCardsShareRowHeight()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true);
    window.resize(1200, 800);
    window.show();
    QCoreApplication::processEvents();

    QStackedWidget* pages = requireControl<QStackedWidget>(window,
        QStringLiteral("settingsPages"));
    const auto card = [](QWidget* page, const QString& title) -> QFrame*
    {
        for (QLabel* label : page->findChildren<QLabel*>())
            if (label->property("cardTitle").toBool() && label->text() == title)
                return qobject_cast<QFrame*>(label->parentWidget());
        throw std::runtime_error(("Missing card: " + title).toStdString());
    };
    QFrame* hardware = card(pages->widget(0), QStringLiteral("Hardware"));
    QFrame* behavior = card(pages->widget(0), QStringLiteral("General behavior"));
    QFrame* display = card(pages->widget(0), QStringLiteral("Display"));
    QFrame* input = card(pages->widget(0), QStringLiteral("Input processing"));
    require(hardware->height() == behavior->height() && hardware->y() == behavior->y(),
        "The first two-column card row is not height-aligned");
    require(display->height() == input->height() && display->y() == input->y(),
        "The second two-column card row is not height-aligned");
    require((hardware->layout()->alignment() & Qt::AlignTop) != 0 &&
        (display->layout()->alignment() & Qt::AlignTop) != 0,
        "Two-column card contents are not top-justified");
    require(hardware->findChild<QComboBox*>(
        QStringLiteral("config.general.renderer")) == nullptr &&
        display->findChild<QComboBox*>(
        QStringLiteral("config.general.renderer")) != nullptr,
        "The renderer selector was not moved from Hardware to Display");
    require(display->findChild<QCheckBox*>(
        QStringLiteral("config.general.hide_legacy_renderers")) != nullptr,
        "Display does not contain the Hide legacy renderers checkbox");
    QCheckBox* videoOnly = requireControl<QCheckBox>(window,
        QStringLiteral("config.general.noui"));
    require(videoOnly->text() == QStringLiteral("Start as Video Only"),
        "The Video Only startup checkbox does not use its explicit label");

    pages->setCurrentIndex(6);
    QCoreApplication::processEvents();
    QFrame* application = card(pages->widget(6), QStringLiteral("Application"));
    QFrame* capture = card(pages->widget(6), QStringLiteral("Capture & renderer"));
    require(application->height() == capture->height() &&
        application->y() == capture->y(),
        "The Shortcuts cards are not height-aligned");
    require((application->layout()->alignment() & Qt::AlignTop) != 0 &&
        (capture->layout()->alignment() & Qt::AlignTop) != 0,
        "The Shortcuts card contents are not top-justified");
    QLineEdit* populatedShortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.capture_3"));
    QLineEdit* emptyShortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.video_conversion_off"));
    require(populatedShortcut->width() == emptyShortcut->width() &&
        populatedShortcut->minimumWidth() == emptyShortcut->minimumWidth() &&
        populatedShortcut->maximumWidth() == emptyShortcut->maximumWidth(),
        "Empty shortcut editors do not retain the populated field width");
    QStringList shortcutLabels;
    for (QLabel* label : pages->widget(6)->findChildren<QLabel*>())
        shortcutLabels.append(label->text());
    require(shortcutLabels.contains(QStringLiteral("Screenshot")) &&
        shortcutLabels.contains(QStringLiteral("Re-apply rules")) &&
        shortcutLabels.contains(QStringLiteral("Show profiles")) &&
        !shortcutLabels.contains(QStringLiteral("Capture rendered output")) &&
        shortcutLabels.contains(QStringLiteral("Video conversion off")) &&
        shortcutLabels.contains(QStringLiteral("V210 to P010 conversion")) &&
        !shortcutLabels.contains(QStringLiteral("Active renderer: conversion off")) &&
        !shortcutLabels.contains(QStringLiteral("Active renderer: V210 to P010")),
        "Shared video-conversion shortcuts do not use compact renderer-neutral labels");
}

void testInheritedRendererInputSelectorsUseEffectiveLabels()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true);
    selectData(requireControl<QComboBox>(window, "config.general.video_conversion"), "V210_TO_P010");
    selectData(requireControl<QComboBox>(window, "config.general.hdr_colorspace"), "FOLLOW_INPUT_LLDV");
    selectData(requireControl<QComboBox>(window, "config.directshow.video_conversion"), QString());
    selectData(requireControl<QComboBox>(window, "config.directshow.hdr_colorspace"), QString());
    const auto verifyInherited = [&window](const QString& objectName,
        const QString& expectedText)
    {
        QComboBox* combo = requireControl<QComboBox>(window, objectName);
        require(combo->currentData().toString().isEmpty(),
            "An inherited input setting must retain its empty override value");
        require(combo->currentText() == expectedText,
            QStringLiteral("An inherited input setting did not identify its effective value: %1 is '%2', expected '%3'")
                .arg(objectName, combo->currentText(), expectedText).toStdString().c_str());
        require(combo->property("inherited").toBool(),
            "An inherited input setting is not styled as inherited");
    };
    verifyInherited(QStringLiteral("config.directshow.video_conversion"),
        QStringLiteral("Inherited: V210 to P010"));
    verifyInherited(QStringLiteral("config.vprenderer.input_processing.video_conversion"),
        QStringLiteral("Inherited: V210 to P010"));
    verifyInherited(QStringLiteral("config.directshow.hdr_colorspace"),
        QStringLiteral("Inherited: Follow input (LLDV)"));

    QComboBox* override = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.input_processing.hdr_colorspace"));
    selectData(override, QStringLiteral("FOLLOW_INPUT"));
    require(override->itemText(0) == QStringLiteral("Inherited: Follow input (LLDV)"),
        "The inherit choice did not identify General's effective value beside an override");
}

void testInheritedRendererInputSelectorsRefreshWithGeneral()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true);
    selectData(requireControl<QComboBox>(window, "config.directshow.container_colorspace"), QString());
    selectData(requireControl<QComboBox>(window, "config.directshow.hdr_luminance"), QString());
    QComboBox* generalContainer = requireControl<QComboBox>(window,
        QStringLiteral("config.general.container_colorspace"));
    selectData(generalContainer, QString());
    for (const QString& objectName : { QStringLiteral("config.directshow.container_colorspace"),
        QStringLiteral("config.vprenderer.input_processing.container_colorspace") })
    {
        QComboBox* inherited = requireControl<QComboBox>(window, objectName);
        require(inherited->currentData().toString().isEmpty() &&
            inherited->currentText() == QStringLiteral("Inherited: Follow input"),
            "An inherited container policy did not expose General's Follow input value");
    }

    QComboBox* generalHdr = requireControl<QComboBox>(window,
        QStringLiteral("config.general.hdr_luminance"));
    selectData(generalHdr, QStringLiteral("FOLLOW_INPUT"));
    for (const QString& objectName : { QStringLiteral("config.directshow.hdr_luminance"),
        QStringLiteral("config.vprenderer.input_processing.hdr_luminance") })
    {
        QComboBox* inherited = requireControl<QComboBox>(window, objectName);
        require(inherited->currentData().toString().isEmpty() &&
            inherited->currentText() == QStringLiteral("Inherited: Follow input"),
            "An inherited HDR policy did not refresh after its General value changed");
    }

    QComboBox* hdrOverride = requireControl<QComboBox>(window,
        QStringLiteral("config.directshow.hdr_colorspace"));
    selectData(hdrOverride, QStringLiteral("FOLLOW_INPUT_LLDV"));
    QComboBox* generalColor = requireControl<QComboBox>(window,
        QStringLiteral("config.general.hdr_colorspace"));
    selectData(generalColor, QStringLiteral("FOLLOW_INPUT"));
    require(hdrOverride->itemText(0) == QStringLiteral("Inherited: Follow input"),
        "The inherit choice did not refresh beside an active renderer override");
}

void testSdrGammaAdjustmentLabelsAndPersistence()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    {
        ConfigEditorWindow window(path, 0, true);
        auto* adjustment = requireControl<QCheckBox>(window, "config.vprenderer.color.sdr_adjust_gamma");
        auto* desired = requireControl<QComboBox>(window, "config.vprenderer.color.sdr_input_transfer");
        require(window.findChild<QWidget*>("config.vprenderer.color.calibration_lut_input_gamma") == nullptr,
            "Color / Output still exposes a saved LUT compatibility control");
        require(requireControl<QComboBox>(window, "config.vprenderer.color.hdr_tone_map_target_gamma")->accessibleName() == "HDR LUT input gamma",
            "Rendering HDR target gamma label is unclear");
        require(adjustment->accessibleName() == "Enable SDR gamma processing", "Missing gamma checkbox label");
        const int automatic = desired->findData("AUTO", Qt::UserRole, Qt::MatchFixedString);
        if (automatic >= 0)
            require(!(desired->model()->flags(desired->model()->index(automatic, 0)) & Qt::ItemIsEnabled),
                "Saved automatic assumption is offered as a new choice");
        adjustment->setCheckState(Qt::Checked);
        require(desired->isEnabled(), "Desired gamma disabled with processing on");
        adjustment->setCheckState(Qt::Unchecked);
        require(desired->isEnabled(), "Expected source gamma must remain enabled with display conversion off");
        save(window);
    }
    require(readBytes(path).contains("sdr_adjust_gamma: passthrough"), "Unchecked checkbox did not save passthrough");
    {
        ConfigEditorWindow window(path, 0, true);
        require(requireControl<QCheckBox>(window, "config.vprenderer.color.sdr_adjust_gamma")->checkState() == Qt::Unchecked,
            "Pass-through did not reload unchecked");
    }
    for (const QByteArray mode : { QByteArray("off"), QByteArray("AUTO") })
    {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write beta fixture");
        file.write("[general]\nrenderer: VP Renderer\n[vprenderer.Default]\nquality: high\n"
            "[vprenderer.color.First]\nsdr_adjust_gamma: " + mode +
            "\nsdr_input_transfer: AUTO\noutput_gamma: bt1886 # preserve calibration comment\n"
            "hdr_tone_map_target_gamma: bt1886\n"
            "[vprenderer.color.Second]\noutput_gamma: 2.2\n");
        file.close();
        ConfigEditorWindow window(path, 0, true);
        auto* adjustment = requireControl<QCheckBox>(window, "config.vprenderer.color.sdr_adjust_gamma");
        require(adjustment->checkState() == Qt::Unchecked, "Beta adjustment was not normalized");
        require(requireControl<QComboBox>(window, "config.vprenderer.color.output_gamma")->currentData() == "2.4",
            "BT.1886 display gamma was not translated");
        requireControl<QListWidget>(window, "config.vprenderer.color.profiles")->setCurrentRow(1);
        require(adjustment->checkState() == Qt::Unchecked, "Inherited adjustment was not normalized");
        save(window);
        const auto saved = readBytes(path);
        require(saved.contains("sdr_adjust_gamma: passthrough") && saved.contains("sdr_input_transfer: 2.4") &&
            saved.contains("output_gamma: 2.4 # preserve calibration comment") &&
            saved.contains("hdr_tone_map_target_gamma: 2.4") && saved.contains("# SDR gamma update:"),
            "Beta normalization or its report did not persist");
        ConfigEditorWindow reloaded(path, 0, true);
        require(!requireControl<QPushButton>(reloaded, "applyConfiguration")->isEnabled(), "Migration repeats on reload");
    }

}

void testCalibrationControlsFollowConfirmedRuntimeLut()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    auto* vp = requireControl<QRadioButton>(window, "config.vprenderer.color.calibration_method.vp");
    auto* lut = requireControl<QRadioButton>(window, "config.vprenderer.color.calibration_method.lut");
    auto* display = requireControl<QComboBox>(window, "config.vprenderer.color.output_gamma");
    auto* source = requireControl<QComboBox>(window, "config.vprenderer.color.sdr_input_transfer");
    auto* response = requireControl<QComboBox>(window, "config.vprenderer.color.sdr_response");
    auto* hdr = requireControl<QComboBox>(window, "config.vprenderer.color.hdr_tone_map_target_gamma");
    auto* fallback = requireControl<QPushButton>(window, "config.vprenderer.color.edit_fallback");
    auto* status = requireControl<QLabel>(window, "config.vprenderer.color.lut_attachment_status");
    auto* gamutLabel = requireControl<QLabel>(window, "config.vprenderer.color.target_primaries.label");
    window.selectPage(16);
    window.resize(1500, 1100);
    window.show();
    QCoreApplication::processEvents();
    vp->click();
    selectData(display, "2.2");
    selectData(response, "2.4");
    require(source->currentData().toString() == "2.4", "Combined SDR gamma failed to set the reference");
    selectData(response, "passthrough");
    require(source->currentData().toString() == "2.4", "No gamma adjustment discarded the remembered gamma");
    save(window);
    require(readBytes(path).contains("sdr_adjust_gamma: passthrough"), "No-adjustment choice did not persist");
    selectData(response, "2.4");
    require(display->isVisibleTo(&window) && response->isVisibleTo(&window) && !source->isVisibleTo(&window) &&
        !hdr->isVisibleTo(&window) && gamutLabel->text() == "Display calibrated gamut",
        "VP mode is not a compact display-only editor");
    const QString captures = qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
    const auto captureCalibration = [&](const QString& file)
    {
        auto* section = requireControl<QWidget>(window, "rendererSection.calibration.content")->parentWidget();
        for (QWidget* parent = section->parentWidget(); parent; parent = parent->parentWidget())
            if (auto* scroll = qobject_cast<QScrollArea*>(parent)) { scroll->ensureWidgetVisible(section); break; }
        QCoreApplication::processEvents();
        window.grab().copy(QRect(section->mapTo(&window, QPoint()), section->size())).save(file);
    };

    if (!captures.isEmpty())
    {
        QCoreApplication::processEvents();
        captureCalibration(captures + "/vp.png");
    }
    auto* calibrationSection = requireControl<QWidget>(window, "rendererSection.calibration.content");
    auto* gamut = requireControl<QComboBox>(window, "config.vprenderer.color.target_primaries");
    const int valueColumn = gamut->mapTo(calibrationSection, QPoint()).x();
    lut->click();
    QCoreApplication::processEvents();
    require(gamut->mapTo(calibrationSection, QPoint()).x() == valueColumn, "Changing calibration mode shifted the value column");
    require(lut->isChecked() && !vp->isChecked(), "Calibration radio buttons are not exclusive");
    require(!display->isVisibleTo(&window) && !response->isVisibleTo(&window) && source->isVisibleTo(&window) &&
        hdr->isVisibleTo(&window) && gamutLabel->text() == "LUT input gamut", "LUT mode retained irrelevant display controls");
    selectData(hdr, "2.2");
    require(source->currentData().toString() == "2.4", "HDR LUT gamma changed the SDR reference");
    auto* sdrLutGamma = requireControl<QComboBox>(window, "config.vprenderer.color.sdr_lut_input_gamma");
    require(sdrLutGamma->currentText() == "No gamma conversion",
        "Omitted SDR LUT gamma has an ambiguous or HDR-specific default label");
    if (!captures.isEmpty())
    {
        QCoreApplication::processEvents();
        captureCalibration(captures + "/lut.png");
    }
    save(window);
    const QByteArray beforeLive = readBytes(path);
    const quint64 identity = ConfigurationIdentity::FromText(beforeLive.toStdString());
    const QString color = display->property("profileSection").toString();
    const QString rendering = requireControl<QComboBox>(window, "config.vprenderer.quality")->property("profileSection").toString();
    for (bool attached : { false, true })
        for (bool hdrSource : { false, true })
        {
            window.setCalibrationStatusForTesting(true, attached, identity, path, rendering, color, hdrSource);
            require(!display->isVisibleTo(&window) && !response->isVisibleTo(&window) &&
                source->isEnabled() && hdr->isEnabled(), "Playback state changed the LUT editing layout");
            require(status->text().contains(attached ? "Attached" : "Not attached"), "Confirmed LUT status is incorrect");
        }
    window.setCalibrationStatusForTesting(false, false, 0, path, rendering, color, false);
    require(status->text().contains("unavailable"), "Unavailable status is reported as LUT failure");
    fallback->click();
    require(display->isVisibleTo(&window) && response->isVisibleTo(&window) && lut->isChecked(),
        "Fallback editing switched mode or left the fallback inaccessible");
    require(display->isEnabled() && response->isEnabled(), "Runtime attachment blocked editing fallback settings");
    fallback->click();
    require(!requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(), "Opening fallback marked the config dirty");
    require(readBytes(path) == beforeLive, "Changing live status or opening fallback rewrote configuration");
    vp->click();
    require(response->currentData().toString() == "2.4" && display->currentData().toString() == "2.2" &&
        hdr->currentData().toString() == "2.2", "Switching modes lost independent gamma values");
    save(window);
    ConfigEditorWindow reloaded(path, 0, true);
    require(requireControl<QRadioButton>(reloaded, "config.vprenderer.color.calibration_method.vp")->isChecked() &&
        requireControl<QComboBox>(reloaded, "config.vprenderer.color.sdr_response")->currentData().toString() == "2.4",
        "Radio mode or combined SDR gamma failed save/reload");
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.color.profiles");
    require(profiles->count() > 1, "Fixture lacks an inherited calibration profile");
    profiles->setCurrentRow(1);
    selectData(response, "2.6");
    require(requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(),
        (QStringLiteral("Named gamma edit could not save: ") + requireControl<QLabel>(window, "configurationStatus")->text()).toStdString().c_str());
    save(window);
    selectData(response, "__inherit");
    require(requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(),
        (QStringLiteral("Restoring inheritance could not save: ") + requireControl<QLabel>(window, "configurationStatus")->text()).toStdString().c_str());
    save(window);
    require(response->currentData().toString() == "2.4", "Combined SDR response did not restore profile inheritance");
    requireControl<QPushButton>(window, "config.vprenderer.color.luminance_link")->click();
    require(requireControl<QStackedWidget>(window, "settingsPages")->currentIndex() == 2 &&
        requireControl<QToolButton>(window, "rendererSection.toneMapping")->isChecked(), "Luminance link did not reveal Rendering controls");
}

void testIndependentSdrLutGamma()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    {
        ConfigEditorWindow window(path, 0, true);
        auto combo = [&](const char* key) { return requireControl<QComboBox>(window, QString("config.vprenderer.color.") + key); };
        require(combo("sdr_response")->currentData() == "passthrough", "Fresh SDR conversion is not opt-in");
        for (const char* key : { "sdr_response", "sdr_input_transfer", "output_gamma", "hdr_tone_map_target_gamma", "sdr_lut_input_gamma" })
            require(combo(key)->findData("bt1886") < 0, "BT.1886 remains selectable");
        auto* lutGamma = combo("sdr_lut_input_gamma");
        auto* hdrGamma = combo("hdr_tone_map_target_gamma");
        require(lutGamma->currentData() == "passthrough", "SDR LUT conversion is not opt-in");
        requireControl<QRadioButton>(window, "config.vprenderer.color.calibration_method.lut")->click();
        QCoreApplication::processEvents();
        require(!lutGamma->isHidden(), "LUT mode does not expose SDR LUT input gamma");
        selectData(combo("sdr_input_transfer"), "2.4");
        selectData(lutGamma, "2.2");
        selectData(hdrGamma, "2.6");
        require(combo("sdr_input_transfer")->currentData() == "2.4" && lutGamma->currentData() == "2.2",
            "HDR edit changed the SDR contract");
        selectData(lutGamma, "2.4");
        require(hdrGamma->currentData() == "2.6", "SDR edit changed HDR gamma");
        selectData(lutGamma, "2.2");
        save(window);
    }
    const auto saved = readBytes(path);
    require(saved.contains("sdr_lut_input_gamma: 2.2") && saved.contains("hdr_tone_map_target_gamma: 2.6") &&
        saved.contains("sdr_input_transfer: 2.4"), "Separate gamma settings did not persist");
    ConfigEditorWindow reloaded(path, 0, true);
    require(requireControl<QComboBox>(reloaded, "config.vprenderer.color.sdr_lut_input_gamma")->currentData() == "2.2",
        "SDR LUT gamma failed to reload");
}

void testCalibrationProfileOwnsLutAndHdrGamma()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create calibration ownership fixture");
    file.write("[general]\nrenderer: VP Renderer\nswitch_refresh_rate: fullscreen_only\n"
        "[vprenderer.Default]\nquality: high\nsdr_target_nits: 100\nsdr_black_nits: 0\n"
        "[vprenderer.Other]\nquality: fast\n"
        "[vprenderer.color.First]\ntarget_primaries: rec709\ncalibration_lut_enabled: true\n"
        "calibration_lut_bt709: luts/First.cube\ncalibration_lut_p3_d65: none\n"
        "calibration_lut_bt2020: none\nhdr_tone_map_target_gamma: 2.2\n"
        "[vprenderer.color.Second]\ntarget_primaries: p3_d65\ncalibration_lut_enabled: false\n"
        "calibration_lut_bt709: none\ncalibration_lut_p3_d65: luts/Second.cube\n"
        "hdr_tone_map_target_gamma: 2.4\n"
        "[vprenderer.color.Third]\ntarget_primaries: bt2020\ncalibration_lut_enabled: false\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* pages = requireControl<QStackedWidget>(window, "settingsPages");
    auto* colorProfiles = requireControl<QListWidget>(window, "config.vprenderer.color.profiles");
    auto* renderingProfiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* enabled = requireControl<QCheckBox>(window, "config.vprenderer.color.calibration_lut_enabled");
    auto* slot = requireControl<QComboBox>(window, "config.vprenderer.color.calibration_lut_bt709");
    auto* p3 = requireControl<QComboBox>(window, "config.vprenderer.color.calibration_lut_p3_d65");
    auto* gamma = requireControl<QComboBox>(window, "config.vprenderer.color.hdr_tone_map_target_gamma");
    auto* section = requireControl<QWidget>(window, "rendererSection.calibration.content");
    require(pages->widget(16)->isAncestorOf(section) && section->isAncestorOf(gamma),
        "Calibration LUTs and their HDR input gamma are not in one Color / Output section");
    require(pages->widget(2)->findChild<QWidget*>("rendererSection.calibration.content") == nullptr &&
        window.findChild<QWidget*>("config.vprenderer.hdr_tone_map_target_gamma") == nullptr &&
        window.findChild<QWidget*>("config.vprenderer.calibration_lut_enabled") == nullptr,
        "Rendering still exposes a duplicate calibration control");
    require(pages->widget(2)->isAncestorOf(requireControl<QLineEdit>(window, "config.vprenderer.sdr_target_nits")) &&
        pages->widget(2)->isAncestorOf(requireControl<QLineEdit>(window, "config.vprenderer.sdr_black_nits")),
        "General tone-map luminance controls moved away from Rendering");
    QCoreApplication::processEvents();
    require(enabled->isChecked() && slot->isEnabled() && slot->currentData().toString() == "luts/First.cube" &&
        gamma->isEnabled() && gamma->currentData().toString() == "2.2", "First calibration profile did not load its complete contract");
    renderingProfiles->setCurrentRow(1);
    require(slot->currentData().toString() == "luts/First.cube" && gamma->currentData().toString() == "2.2",
        "Selecting Rendering changed the Color calibration contract");
    colorProfiles->setCurrentRow(1);
    require(!enabled->isChecked() && !slot->isEnabled() && slot->currentData().toString() == "none" &&
        p3->currentData().toString() == "luts/Second.cube" && !gamma->isEnabled() && gamma->currentData().toString() == "2.4",
        "Second calibration profile did not switch files, enablement and HDR gamma together");
    colorProfiles->setCurrentRow(2);
    require(!enabled->isChecked() && !slot->isEnabled() && !gamma->isEnabled(),
        "Switching between disabled profiles re-enabled LUT controls");
    require(slot->currentData().toString().isEmpty() && slot->property("effectiveValue").toString() == "luts/First.cube",
        "A omitted LUT slot did not inherit from the default Color profile");
    enabled->click();
    selectData(slot, "none");
    save(window);
    ConfigEditorWindow reloaded(path, 0, true);
    requireControl<QListWidget>(reloaded, "config.vprenderer.color.profiles")->setCurrentRow(2);
    auto* reloadedSlot = requireControl<QComboBox>(reloaded, "config.vprenderer.color.calibration_lut_bt709");
    require(reloadedSlot->currentData().toString() == "none" && reloadedSlot->currentText() == "None",
        "Explicit None resurrected the inherited LUT after saving");
    ConfigEditorCore::ConfigDocument saved;
    std::wstring error;
    require(saved.Load(path.toStdWString(), error), "Cannot read saved calibration ownership fixture");
    require(saved.Get("vprenderer.color.Third", "calibration_lut_bt709") == "none" &&
        saved.Get("vprenderer.color.Second", "calibration_lut_p3_d65") == "luts/Second.cube" &&
        saved.Get("vprenderer.Default", "hdr_tone_map_target_gamma").empty() &&
        saved.Get("vprenderer.Other", "calibration_lut_enabled").empty(),
        "Saving leaked calibration values into Rendering or discarded another profile's LUT");
}

void testHdrTargetGammaLoadsPriorInputAliases()
{
    for (const QByteArray prior : { QByteArray("display"), QByteArray("2.4") })
    {
        QTemporaryDir directory;
        const QString path = directory.filePath("VideoProcessor.cfg");
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "Cannot create prior LUT input fixture");
        file.write("[general]\nrenderer: VP Renderer\nswitch_refresh_rate: fullscreen_only\n"
            "[vprenderer.Default]\nquality: high\ncalibration_lut_input_transfer: " + prior +
            "\n[vprenderer.Other]\nquality: high\n[vprenderer.color.Default]\n"
            "sdr_target_primaries: p3_d65\noutput_gamma: 2.2\ncalibration_lut_input_gamma: 2.6\n[vprenderer.color.Other]\noutput_gamma: 2.4\n");
        file.close();
        ConfigEditorWindow window(path, 0, true);
        auto* hdr = requireControl<QComboBox>(window, "config.vprenderer.color.hdr_tone_map_target_gamma");
        const QString expected = prior == "display" ? QStringLiteral("2.2") : QStringLiteral("2.4");
        require(hdr->currentData().toString() == expected, "Prior Rendering LUT input did not load as explicit HDR gamma");
        auto* gamut = requireControl<QComboBox>(window, "config.vprenderer.color.target_primaries");
        require(gamut->currentData().toString().compare("p3_d65", Qt::CaseInsensitive) == 0,
            "Prior target primaries did not load in Target gamut");
        requireControl<QListWidget>(window, "config.vprenderer.color.profiles")->setCurrentRow(1);
        require(hdr->property("effectiveValue").toString() == expected, "Prior input gamma inheritance was lost");
        selectData(hdr, "2.6");
        save(window);
        const QByteArray saved = readBytes(path);
        ConfigEditorCore::ConfigDocument persisted;
        std::wstring error;
        require(persisted.Load(path.toStdWString(), error), "Cannot reload HDR gamma alias fixture");
        const std::string selectedSection = hdr->property("profileSection").toString().toStdString();
        const auto selectedSettings = persisted.SectionSettings(selectedSection);
        const auto canonical = std::find_if(selectedSettings.begin(), selectedSettings.end(), [](const auto& setting)
        { return setting.first == "hdr_tone_map_target_gamma"; });
        require(canonical != selectedSettings.end() && canonical->second == "2.6",
            (std::string("HDR target gamma did not save canonically in ") + selectedSection +
            "; current=" + persisted.Get(selectedSection.c_str(), "hdr_tone_map_target_gamma") +
            "; selected=" + hdr->currentData().toString().toStdString()).c_str());
        require(saved.contains("calibration_lut_input_gamma: 2.6"), "Ignored Color compatibility value was needlessly discarded");
    }
}

void testLegacyRendererColorSettingsMigrateToColorConfigs()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QByteArray configuration = readBytes(path);
    const qsizetype colorStart = configuration.indexOf(
        "[vprenderer.color.rec709]\n");
    const qsizetype viewportStart = configuration.indexOf(
        "[vprenderer.viewport]\n", colorStart);
    require(colorStart >= 0 && viewportStart > colorStart,
        "The Color Config fixture cannot be prepared for legacy migration");
    configuration.remove(colorStart, viewportStart - colorStart);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        file.write(configuration) == configuration.size(),
        "Cannot prepare a legacy Rendering color-settings fixture");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    QListWidget* colorProfiles = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.color.profiles"));
    require(colorProfiles->count() == 2,
        "Legacy Rendering color settings did not populate independent Color Config profiles");
    require(colorProfiles->item(0)->data(Qt::UserRole).toString() ==
            QStringLiteral("vprenderer.color.rec709") &&
        colorProfiles->item(1)->data(Qt::UserRole).toString() ==
            QStringLiteral("vprenderer.color.bt2020"),
        "Color Config migration did not preserve the legacy profile order");
    save(window);

    const QByteArray saved = readBytes(path);
    const qsizetype colorRec709Start = saved.indexOf(
        "[vprenderer.color.rec709]\n");
    const qsizetype colorRec709End = saved.indexOf("\n[", colorRec709Start + 1);
    const QByteArray rec709Color = saved.mid(colorRec709Start,
        colorRec709End - colorRec709Start);
    require(colorRec709Start >= 0 && colorRec709End > colorRec709Start &&
        saved.contains("[vprenderer.color.bt2020]\n") &&
        rec709Color.contains("when: ${key}==\"F5\"") &&
        !rec709Color.contains("sdr_target_nits:"),
        "Color Config migration incorrectly moved renderer nits into Color Config");
    const qsizetype rendererStart = saved.indexOf("[vprenderer.rec709]\n");
    const qsizetype rendererEnd = saved.indexOf("[vprenderer.bt2020]\n",
        rendererStart);
    const QByteArray renderer = saved.mid(rendererStart,
        rendererEnd - rendererStart);
    require(rendererStart >= 0 && rendererEnd > rendererStart &&
        renderer.contains("sdr_target_nits: 100") &&
        !renderer.contains("sdr_target_primaries:"),
        "Renderer nits did not remain in the Rendering profile");
}

void testLegacyVpInputOverrideMigratesToIndependentPolicySection()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QFile file(path);
    require(file.open(QIODevice::Append),
        "Could not append the legacy VP input override fixture");
    file.write("\n[vprenderer]\nvideo_conversion: NONE\n");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    QComboBox* conversion = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.input_processing.video_conversion"));
    require(conversion->currentData().toString() == QStringLiteral("NONE"),
        "The legacy VP input override was not loaded into the new selector");
    save(window);

    const QByteArray saved = readBytes(path);
    const int inputSection = saved.indexOf("[vprenderer.input_processing]");
    const int inputEnd = saved.indexOf(QByteArray("\n["), inputSection + 1);
    const QByteArray inputValues = inputSection >= 0 ? saved.mid(inputSection,
        inputEnd < 0 ? -1 : inputEnd - inputSection) : QByteArray();
    require(inputValues.contains("video_conversion: NONE"),
        "The migrated VP input override was not saved independently");
    require(!saved.contains("[vprenderer]\nvideo_conversion:"),
        "The legacy display-profile root still owns VP input processing");
    require(!saved.contains("[vprenderer.profile_1]"),
        "Migrating input policy created a synthetic display profile");
}

void testLldvNamedMetadataIsPreserved()
{
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create LLDV migration directory");
    const QString path = copyFixture(directory);
    QByteArray config = readBytes(path);
    config.replace("hdr_colorspace: FOLLOW_INPUT_LLDV", "hdr_colorspace: FOLLOW_INPUT");
    config.replace("hdr_luminance: FOLLOW_INPUT_LLDV", "hdr_luminance: FOLLOW_INPUT");
    config.replace("[lldv]", "[lldv.default]");
    config.replace("#max_cll: 1000", "max_cll: 1234");
    config.replace("#max_fall: 401", "max_fall: 432");
    config.replace("#mastering_min_luminance: 0.001",
        "mastering_min_luminance: 0.002");
    config.replace("#mastering_max_luminance: 4000",
        "mastering_max_luminance: 3500");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Cannot write LLDV migration fixture");
    require(file.write(config) == config.size(),
        "Cannot replace LLDV migration fixture");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    require(window.findChild<QCheckBox*>(QStringLiteral("config.lldv.enabled")) == nullptr,
        "LLDV metadata handling is always enabled and must not be exposed as a toggle");
    require(requireControl<QLineEdit>(window,
        QStringLiteral("config.lldv.max_cll"))->text() == QStringLiteral("1234"),
        "Named LLDV profile did not retain MaxCLL");

    save(window);
    const QByteArray saved = readBytes(path);
    require(!saved.contains("[lldv]") && saved.contains("[lldv.default]"),
        "Named LLDV profile was unexpectedly renamed");
    require(saved.contains("hdr_colorspace: FOLLOW_INPUT_LLDV") &&
        saved.contains("hdr_luminance: FOLLOW_INPUT_LLDV"),
        "Explicit LLDV metadata remained silently disabled by FOLLOW_INPUT");
}

void testOptionalSectionsAreCreatedWhenEdited()
{
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create optional-section test directory");
    const QString path = copyFixture(directory);
    QByteArray config = readBytes(path);
    const auto removeSection = [&config](const QByteArray& section)
    {
        const int start = config.indexOf(section);
        require(start >= 0, "Optional-section fixture is missing a section");
        const int next = config.indexOf("\n[", start + section.size());
        config.remove(start, next < 0 ? config.size() - start : next - start + 1);
    };
    removeSection("[lldv]\n");
    removeSection("[logging]\n");
    removeSection("[shortcuts]\n");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Cannot write optional-section fixture");
    require(file.write(config) == config.size(),
        "Cannot replace optional-section fixture");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    requireControl<QPushButton>(window, QStringLiteral("config.lldv.add_profile"))->click();
    requireControl<QLineEdit>(window, QStringLiteral("config.lldv.max_cll"))
        ->setText(QStringLiteral("1200"));
    requireControl<QLineEdit>(window, QStringLiteral("config.lldv.max_fall"))
        ->setText(QStringLiteral("400"));
    requireControl<QLineEdit>(window,
        QStringLiteral("config.lldv.mastering_min_luminance"))
        ->setText(QStringLiteral("0.002"));
    requireControl<QLineEdit>(window,
        QStringLiteral("config.lldv.mastering_max_luminance"))
        ->setText(QStringLiteral("3500"));
    requireControl<QCheckBox>(window, QStringLiteral("config.logging.debug"))
        ->setChecked(true);
    requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.fullscreen_toggle"))
        ->setText(QStringLiteral("Ctrl+Alt+F"));
    save(window);

    const QByteArray saved = readBytes(path);
    const int lldvStart = saved.toLower().indexOf("[lldv.new_1]\n");
    const int lldvEnd = saved.indexOf("\n[", lldvStart + 1);
    const QByteArray lldv = saved.mid(lldvStart,
        lldvEnd < 0 ? -1 : lldvEnd - lldvStart);
    require(lldvStart >= 0 && lldv.contains("max_cll: 1200") &&
        lldv.contains("max_fall: 400") &&
        lldv.contains("mastering_min_luminance: 0.002") &&
        lldv.contains("mastering_max_luminance: 3500"),
        "Editing LLDV metadata did not create and populate a named LLDV profile");
    require(saved.contains("[logging]\ndebug: true"),
        "Editing a logging control did not create the [logging] section");
    require(saved.contains("[shortcuts]\nfullscreen_toggle: Ctrl+Alt+F"),
        "Editing a shortcut did not create the [shortcuts] section");
}

void answerInputDialog(const QString& text)
{
    auto* timer = new QTimer(qApp);
    timer->setInterval(1);
    QObject::connect(timer, &QTimer::timeout, qApp, [timer, text]
    {
        for (QWidget* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QInputDialog*>(widget))
            {
                dialog->setTextValue(text);
                dialog->accept();
                timer->stop();
                timer->deleteLater();
                return;
            }
    });
    timer->start();
}

void answerMessageBox(int result)
{
    auto* timer = new QTimer(qApp);
    timer->setInterval(1);
    QObject::connect(timer, &QTimer::timeout, qApp, [timer, result]
    {
        for (QWidget* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QMessageBox*>(widget))
            {
                if (QAbstractButton* button = dialog->button(
                    static_cast<QMessageBox::StandardButton>(result)))
                    button->click();
                else
                    dialog->done(result);
                timer->stop();
                timer->deleteLater();
                return;
            }
    });
    timer->start();
}

void testIntegratedUpdatesRespectLocalPackage()
{
    QTemporaryDir directory;
    QWidget host; host.resize(1000, 700);
    UpdatePanel panel(directory.path(), &host, false);
    require(!panel.isWindow(), "Updates opened a separate top-level window");
    ConfigEditorWindow window(copyFixture(directory), 0, true);
    auto* entry = window.findChild<QPushButton*>("config.openUpdates");
    require(entry, "Sidebar has no Updates entry"); window.show(); entry->click(); QApplication::processEvents();
    auto* pages = window.findChild<QStackedWidget*>("settingsPages");
    auto* embedded = window.findChild<QWidget*>("config.updates");
    require(embedded && pages && pages->currentWidget()->isAncestorOf(embedded) && !embedded->isWindow(),
        "Updates was not part of the Config settings page stack");
    require(window.findChild<QWidget*>("brandHeader")->isVisible() &&
        window.findChild<QWidget*>("sidebar")->isVisible(), "Updates hid the standard Config navigation");
    window.selectPage(0);
    require(!embedded->isVisible() && pages->currentIndex() == 0, "Settings navigation did not leave Updates");
    window.showUpdates();
    QJsonObject state{{"flavor", "config"}, {"version", "test"}, {"mode", "notify"}, {"channel", "beta"},
        {"status", "No update available"}, {"busy", false}, {"canDownload", false}, {"canInstall", false}};
    panel.applyState(state);
    auto* identity = panel.findChild<QLabel*>("updates.identity");
    require(identity && identity->text().contains("Config only"), "Config-only updates incorrectly require VP");
    auto* mode = panel.findChild<QComboBox*>("updates.mode");
    require(mode && mode->currentData() == "notify" && mode->isEnabled(), "Update mode did not reflect helper settings");
    static_cast<UpdatePanel*>(embedded)->applyState(state);
    const QString captures = qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
    if (!captures.isEmpty()) { QApplication::processEvents(); window.grab().save(QDir(captures).filePath("updates-settings.png")); }
    ConfigEditorWindow offline(directory.filePath(QStringLiteral("VideoProcessor.cfg")), 0, true, {}, {}, {}, 41686, {}, true);
    offline.showUpdates(); QApplication::processEvents();
    require(offline.findChild<QWidget*>("config.updates")->isVisible(), "Updates unavailable without a VP target");
    require(!offline.findChild<QPushButton*>("applyConfiguration")->isEnabled(), "Updates enabled offline configuration writes");

    state["flavor"] = "full"; state["busy"] = true; state["status"] = "Downloading"; panel.applyState(state);
    require(identity->text().contains("VideoProcessor and Config"), "Full package identity was not shown");
    require(!mode->isEnabled(), "Preferences remained mutable during installation operation");
    panel.applyState(QJsonObject{{"error", "Update helper unavailable"}});
    require(panel.findChild<QLabel*>("updates.status")->text().contains("unavailable"), "Helper error was not visible inside Config");
    panel.applyState(QJsonObject{{"error", "Timed out"}, {"connectionError", true}});
    require(panel.findChild<QLabel*>("updates.status")->text().contains("Reconnecting") &&
        !panel.findChild<QLabel*>("updates.status")->text().contains("setup"), "Disconnect incorrectly blamed setup");
    state["busy"] = false; panel.applyState(state);
    require(mode->isEnabled(), "Healthy helper response did not recover the page");

}

void testUpdateShutdownDiscardsEditsWithoutPrompt()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    const QByteArray original = readBytes(path);
    const UINT message = RegisterWindowMessageW(L"VideoProcessor.UpdateExit.v1");
    require(message != 0, "Cannot register updater shutdown request");
    for (bool dirty : { false, true })
    {
        ConfigEditorWindow window(path, 0, true);
        if (dirty)
        {
            auto* field = requireControl<QCheckBox>(window, "config.general.fullscreen");
            field->setChecked(!field->isChecked());
        }
        const HWND handle = reinterpret_cast<HWND>(window.winId());
        window.hide();
        bool prompted = false;
        QTimer dismissUnexpectedPrompt;
        QObject::connect(&dismissUnexpectedPrompt, &QTimer::timeout, [&prompted]
        {
            for (QWidget* widget : QApplication::topLevelWidgets())
                if (auto* dialog = qobject_cast<QMessageBox*>(widget))
                {
                    prompted = true;
                    dialog->done(QMessageBox::Cancel);
                }
        });
        dismissUnexpectedPrompt.start(10);
        for (int attempt = 0; attempt < 2; ++attempt)
            require(SendMessageW(handle, message, 0, 0) == 1,
                "Updater shutdown was not acknowledged");
        require(!prompted, "Updater shutdown prompted about unsaved edits");
        require(window.isHidden(), "Updater shutdown revealed hidden Config");
        require(readBytes(path) == original, "Updater shutdown saved unsaved edits");
        require(SendMessageW(handle, message, 1, 0) == 3,
            "Hidden Config did not report tray-only restart state");
        // Destroy the fixture before its queued application quit.
    }
}

void testDvModeSelectorPreservesLldvSettings()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QByteArray original = readBytes(path);
    original.replace("[lldv]", "[lldv.cinema]\ndolby_vision_tunnel: false");
    original.replace("#max_cll: 1000", "max_cll: 1234");
    original.replace("#max_fall: 401", "max_fall: 432");
    original.replace("fullscreen_monitor_name:", "#fullscreen_monitor_name:");
    original += "\n[lldv.streaming]\ndolby_vision_tunnel: true\n";
    QFile fixture(path);
    require(fixture.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Cannot prepare per-profile DV fixture");
    require(fixture.write(original) == original.size(), "Cannot write DV mode fixture");
    fixture.close();
    ConfigEditorWindow window(path, 0, true);
    window.resize(1180, 1000);
    window.selectPage(5);
    window.show();
    QApplication::processEvents();
    auto* list = requireControl<QListWidget>(window, "config.lldv.profiles");
    auto* tunneling = requireControl<QRadioButton>(window, "config.dv.mode.tunneling");
    auto* lldv = requireControl<QRadioButton>(window, "config.dv.mode.lldv");
    auto* metadata = requireControl<QWidget>(window, "config.dv.lldv_settings");
    require(list->count() == 2 && lldv->isChecked() && metadata->isVisible(),
        "Cinema must open in LLDV mode with its existing fields");
    bool dvNavigation = false;
    for (auto* button : window.findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("DV") && button->isChecked())
            dvNavigation = true;
    require(dvNavigation, "The navigation entry was not renamed to DV");
    const auto capture = [&window](const QString& name)
    {
        const QString captures = qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
        if (captures.isEmpty()) return;
        QDir().mkpath(captures);
        QApplication::processEvents();
        require(window.grab().save(QDir(captures).filePath(name)),
            "Cannot capture per-profile DV preview");
    };
    save(window);
    capture("dv-lldv-preview.png");
    list->setCurrentRow(1);
    require(tunneling->isChecked() && !lldv->isChecked() && !metadata->isVisible(),
        "Streaming did not load its own DV Tunneling mode");
    require(list->isVisible(), "DV Tunneling hid the profile list");
    capture("dv-tunneling-preview.png");

    // Change Streaming only, then verify Cinema still owns its original mode.
    lldv->click();
    list->setCurrentRow(0);
    require(lldv->isChecked() && metadata->isVisible(),
        "Changing Streaming changed Cinema's mode");
    require(requireControl<QLineEdit>(window, "config.lldv.max_cll")->text() == "1234",
        "Changing another profile lost Cinema metadata");
    tunneling->click();
    list->setCurrentRow(1);
    require(lldv->isChecked() && metadata->isVisible(),
        "Changing Cinema changed Streaming's mode");
    save(window);

    ConfigFile config;
    require(config.Load(path.toStdString()), "Cannot load saved DV profiles");
    RendererProfileConfig::Model model;
    std::string error;
    require(RendererProfileConfig::Read(config, model, error), error.c_str());
    require(model.profiles.at("lldv.cinema").settings.at("dolby_vision_tunnel") == "true" &&
        model.profiles.at("lldv.streaming").settings.at("dolby_vision_tunnel") == "false",
        "Profile modes were not saved independently");
    RendererProfileConfig::ResolvedLldv cinemaMode;
    RendererProfileConfig::ResolvedLldv streamingMode;
    require(RendererProfileConfig::ResolveLldv(model, "cinema", cinemaMode, error), error.c_str());
    require(RendererProfileConfig::ResolveLldv(model, "streaming", streamingMode, error), error.c_str());
    require(cinemaMode.UseDolbyVisionTunnel(false) && !streamingMode.UseDolbyVisionTunnel(true),
        "Saved profile selections did not resolve to their runtime modes");
    const QByteArray saved = readBytes(path);
    require(saved.contains("max_cll: 1234") && saved.contains("max_fall: 432") &&
        saved.contains("#mastering_max_luminance: 4000"),
        "Changing profile modes lost LLDV metadata or comments");
    ConfigEditorWindow reopened(path, 0, true);
    auto* reopenedList = requireControl<QListWidget>(reopened, "config.lldv.profiles");
    require(requireControl<QRadioButton>(reopened, "config.dv.mode.tunneling")->isChecked(),
        "Cinema's saved DV Tunneling mode was not restored");
    reopenedList->setCurrentRow(1);
    require(requireControl<QRadioButton>(reopened, "config.dv.mode.lldv")->isChecked(),
        "Streaming's saved LLDV mode was not restored");
}
void testLldvStandardProfiles()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QByteArray original = readBytes(path);
    original.replace("#max_cll: 1000", "max_cll: 1234");
    QFile fixture(path);
    require(fixture.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot update LLDV fixture");
    require(fixture.write(original) == original.size(), "Cannot write LLDV singleton metadata");
    fixture.close();
    ConfigEditorWindow window(path, 0, true);
    auto* list = requireControl<QListWidget>(window, QStringLiteral("config.lldv.profiles"));
    require(list->count() == 1, "Singleton migration must produce exactly one LLDV profile");
    require(list->item(0)->data(Qt::UserRole).toString() == QStringLiteral("lldv.profile_1"),
        "Singleton was not migrated to a standard named profile");
    require(readBytes(path) == original, "Migration changed disk before saving");
    auto* cll = requireControl<QLineEdit>(window, QStringLiteral("config.lldv.max_cll"));
    require(cll->text() == QStringLiteral("1234"), "Singleton migration changed existing metadata");
    requireControl<QLineEdit>(window, QStringLiteral("config.lldv.max_fall"))->setText(QStringLiteral("432"));
    requireControl<QLineEdit>(window, QStringLiteral("config.lldv.mastering_min_luminance"))->setText(QStringLiteral("0.002"));
    requireControl<QLineEdit>(window, QStringLiteral("config.lldv.mastering_max_luminance"))->setText(QStringLiteral("3500"));
    requireControl<QPushButton>(window, QStringLiteral("config.lldv.add_profile"))->click();
    require(list->count() == 2, "Cannot add an LLDV profile");
    cll->setText(QStringLiteral("2000"));
    auto* name = requireControl<QLineEdit>(window, QStringLiteral("config.lldv.name"));
    name->setText(QStringLiteral("Cinema"));
    QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection);
    requireControl<QLineEdit>(window, QStringLiteral("config.lldv.shortcut"))->setText(QStringLiteral("Ctrl+Alt+L"));
    requireControl<QLineEdit>(window, QStringLiteral("config.lldv.cycle_shortcut"))->setText(QStringLiteral("Ctrl+Shift+L"));
    requireControl<QCheckBox>(window, QStringLiteral("config.lldv.use_rule"))->setChecked(true);
    requireControl<QPlainTextEdit>(window, QStringLiteral("config.lldv.when"))->setPlainText(QStringLiteral("${eotf} == \"HDR\""));
    requireControl<QPushButton>(window, QStringLiteral("config.lldv.move_up"))->click();
    require(list->currentRow() == 0, "LLDV reorder failed");
    list->setCurrentRow(1);
    require(cll->text() == QStringLiteral("1234"), "LLDV profiles share edited values");
    save(window);
    const QByteArray saved = readBytes(path).toLower();
    require(!saved.contains("[lldv]"), "Saved LLDV configuration still uses the singleton");
    require(saved.indexOf("[lldv.cinema]") < saved.indexOf("[lldv.profile_1]"),
        "LLDV profile order did not persist");
    require(saved.contains("#mastering_max_luminance: 4000"), "LLDV migration lost comments");
    ConfigFile config;
    require(config.Load(path.toStdString()), "Cannot load saved LLDV profiles");
    RendererProfileConfig::Model model;
    RendererProfileConfig::ResolvedLldv resolved;
    std::string error;
    require(RendererProfileConfig::Read(config, model, error), error.c_str());
    require(RendererProfileConfig::ResolveLldv(model, "cinema", resolved, error), error.c_str());
    require(resolved.hasMaxCll && resolved.maxCll == 2000, "Runtime did not resolve Cinema metadata");
    require(RendererProfileConfig::ResolveLldv(model, "profile_1", resolved, error), error.c_str());
    require(resolved.maxCll == 1234 && resolved.maxFall == 432 &&
        resolved.masteringMinLuminance == 0.002 && resolved.masteringMaxLuminance == 3500,
        "Runtime did not resolve migrated metadata");
    ConfigEditorWindow reopened(path, 0, true);
    require(requireControl<QLineEdit>(reopened, QStringLiteral("config.lldv.cycle_shortcut"))->text() == QStringLiteral("Ctrl+Shift+L"),
        "LLDV cycle shortcut did not persist");
    require(requireControl<QPlainTextEdit>(reopened, QStringLiteral("config.lldv.when"))->toPlainText() == QStringLiteral("${eotf} == \"HDR\""),
        "LLDV rule did not persist");
    auto* reopenedList = requireControl<QListWidget>(reopened, QStringLiteral("config.lldv.profiles"));
    require(reopenedList->count() == 2, "Named LLDV profiles did not survive reopening");
    require(requireControl<QLineEdit>(reopened, QStringLiteral("config.lldv.max_cll"))->text() == QStringLiteral("2000"),
        "First LLDV profile did not retain its own metadata");
    reopenedList->setCurrentRow(1);
    require(requireControl<QLineEdit>(reopened, QStringLiteral("config.lldv.mastering_max_luminance"))->text() == QStringLiteral("3500"),
        "Migrated LLDV metadata did not survive reopening");
    answerMessageBox(QMessageBox::Yes);
    requireControl<QPushButton>(reopened, QStringLiteral("config.lldv.remove_profile"))->click();
    require(reopenedList->count() == 1, "Cannot remove an LLDV profile");
    save(reopened);
}
void testProfileLifecycleThroughWidgets()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    QListWidget* list = requireControl<QListWidget>(window,
        QStringLiteral("config.queue.profiles"));
    require(list->count() == 2, "Queue profiles did not load");

    list->setCurrentRow(1);
    QLineEdit* name = requireControl<QLineEdit>(window,
        QStringLiteral("config.queue.name"));
    name->setText(QStringLiteral("Cinema Queue"));
    QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection);
    require(list->currentItem()->text().startsWith(QStringLiteral("Cinema Queue")),
        "Profile rename did not update the list");

    requireControl<QPushButton>(window, QStringLiteral("config.queue.move_up"))->click();
    require(list->currentRow() == 0, "Move up did not reorder the profile");
    require(list->item(0)->text().contains(QStringLiteral("Default")),
        "First reordered profile was not marked default");

    const int beforeAdd = list->count();
    requireControl<QPushButton>(window, QStringLiteral("config.queue.add_profile"))->click();
    require(list->count() == beforeAdd + 1, "Add profile did not create a profile");
    require(list->currentItem()->text().startsWith(QStringLiteral("New 1")),
        "Add profile did not select the first available generated name");
    require(name->text() == QStringLiteral("New 1"),
        "A generated profile name was not loaded into Profile details");
    name->clear();
    answerMessageBox(QMessageBox::Ok);
    QMetaObject::invokeMethod(name, "editingFinished", Qt::DirectConnection);
    require(name->text() == QStringLiteral("New 1"),
        "Rejecting an empty profile name left the editor blank");
    QCoreApplication::processEvents();

    requireControl<QPushButton>(window, QStringLiteral("config.queue.add_profile"))->click();
    require(list->count() == beforeAdd + 2 &&
        list->currentItem()->text().startsWith(QStringLiteral("New 2")),
        "Add profile did not skip an existing generated name");
    answerMessageBox(QMessageBox::Yes);
    requireControl<QPushButton>(window, QStringLiteral("config.queue.remove_profile"))->click();
    for (int row = 0; row < list->count(); ++row)
        if (list->item(row)->text().startsWith(QStringLiteral("New 1")))
        {
            list->setCurrentRow(row);
            break;
        }
    answerMessageBox(QMessageBox::Yes);
    requireControl<QPushButton>(window, QStringLiteral("config.queue.remove_profile"))->click();
    require(list->count() == beforeAdd, "Remove profile did not remove the selected profile");
    save(window);
    const QByteArray saved = readBytes(path);
    const QByteArray lowerSaved = saved.toLower();
    require(lowerSaved.contains("[queue.cinema_queue]"), "Renamed profile header was not saved");
    require(lowerSaved.indexOf("[queue.cinema_queue]") < lowerSaved.indexOf("[queue.profile_1]"),
        "Reordered profile file order was not saved");
    require(!lowerSaved.contains("new_1"), "Removed profile was serialized");
}

void testUnrelatedContentRemainsExact()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QByteArray original = readBytes(path);
    original.replace("max_core_count: 2\n", "max_core_count: 1\nchroma_downsampling: LEGACY\n");
    {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot prepare manual conversion settings");
        file.write(original);
    }
    const QList<QByteArray> preservedBlocks = {
        "[directshow.conversion]\nconversion_method: SIMD\nmin_core_count: 1\nmax_core_count: 1\nchroma_downsampling: LEGACY",
        "[directshow.ppm]\nppm: auto",
        "# These audio scripts deliberately keep their millisecond delay arguments literal.",
        "# Disabled after VP-0095 topology-mode regression; retained for later retest: fullscreen_monitor_session_mode: target-only"
    };
    for (const QByteArray& block : preservedBlocks)
        require(original.contains(block), "Preservation marker missing from source fixture");

    ConfigEditorWindow window(path, 0, true);
    requireControl<QCheckBox>(window, QStringLiteral("config.general.scene_detect"))->setChecked(false);
    save(window);
    const QByteArray saved = readBytes(path);
    for (const QByteArray& block : preservedBlocks)
        require(saved.contains(block), "An unrelated manual block changed during UI save");
    QByteArray advanced = saved;
    advanced.replace("chroma_downsampling: LEGACY", "chroma_downsampling: ADVANCED");
    {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot prepare ADVANCED settings");
        file.write(advanced);
    }
    ConfigEditorWindow advancedWindow(path, 0, true);
    requireControl<QCheckBox>(advancedWindow, QStringLiteral("config.general.scene_detect"))->setChecked(true);
    save(advancedWindow);
    require(readBytes(path).contains("chroma_downsampling: ADVANCED"), "UI save changed ADVANCED");
    // Hunt 2 (2026-10-17, owner: every setting has a Config control): the chroma policy now has a
    // control; it shows the hand-edited value and an unrelated save keeps it.
    require(requireControl<QComboBox>(advancedWindow, "config.directshow.conversion.chroma_downsampling")
        ->currentData().toString() == "ADVANCED", "The chroma control does not show the hand-edited ADVANCED");
}

void testSceneDetectionDefaultsOffAndHidesManualOverrides()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QByteArray configuration = readBytes(path);
    configuration.replace("scene_detect: true\n", "");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Cannot prepare the scene detection fixture");
    file.write(configuration);
    file.close();

    ConfigEditorWindow window(path, 0, true);
    QCheckBox* sceneDetection = requireControl<QCheckBox>(window,
        QStringLiteral("config.general.scene_detect"));
    require(!sceneDetection->isChecked(),
        "Scene detection should be off when it is absent from configuration");
    require(window.findChild<QCheckBox*>(QStringLiteral(
        "config.general.disable_detection_features")) == nullptr,
        "Manual detection-feature override leaked into the editor UI");
    require(window.findChild<QCheckBox*>(QStringLiteral(
        "config.general.scene_correction_basic")) == nullptr,
        "Basic scene correction leaked into the editor UI");

    sceneDetection->setChecked(true);
    save(window);
    require(readBytes(path).contains("scene_detect: true"),
        "Enabling scene detection did not persist its configured value");
}

void testVirtualShaderOffOptionPersistsWhenConfigured()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QByteArray configuration = readBytes(path);
    const qsizetype rootStart = configuration.indexOf("[shader.nls]\n");
    const qsizetype nextSection = configuration.indexOf("\n[shader.nls.", rootStart);
    require(rootStart >= 0 && nextSection > rootStart,
        "Shader root section was not found in the fixture");
    configuration.remove(rootStart, nextSection - rootStart + 1);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Cannot prepare shader fixture without a default profile section");
    require(file.write(configuration) == configuration.size(),
        "Cannot write shader fixture without a default profile section");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    QListWidget* modes = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.nls.modes"));
    require(modes->count() >= 1 &&
        modes->item(0)->data(Qt::UserRole).toString().compare(
            QStringLiteral("shader.nls"), Qt::CaseInsensitive) != 0,
        "The editor synthesized a fixed Default profile without a root section");
    modes->setCurrentRow(0);
    const QString selectedSection = modes->currentItem()->data(Qt::UserRole).toString();
    QLineEdit* shortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.nls.shortcut"));
    shortcut->setText(QStringLiteral("Ctrl+0"));
    save(window);
    const QByteArray saved = readBytes(path);
    require(!saved.contains("[shader.nls]\n"),
        "Editing the first named profile recreated the removed fixed root");
    const QByteArray header = "[" + selectedSection.toLocal8Bit() + "]";
    const qsizetype selectedStart = saved.indexOf(header);
    const qsizetype selectedEnd = saved.indexOf("\n[", selectedStart + 1);
    const QByteArray selected = saved.mid(selectedStart,
        selectedEnd < 0 ? -1 : selectedEnd - selectedStart);
    require(selected.contains("shortcut: Ctrl+0"),
        "Editing the first named profile did not persist its shortcut");
}

void testShaderProfileListsCanReturnToEmptyWithoutSyntheticRoots()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QByteArray configuration = readBytes(path);
    QList<QByteArray> retained;
    bool skip = false;
    for (const QByteArray& line : configuration.split('\n'))
    {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith('['))
            skip = trimmed.startsWith("[shader.nls") ||
                trimmed.startsWith("[shader.standard");
        if (!skip) retained.push_back(line);
    }
    configuration = retained.join('\n');
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        file.write(configuration) == configuration.size(),
        "Cannot create an empty shader-profile fixture");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    QListWidget* nls = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.nls.modes"));
    QListWidget* standard = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.standard.items"));
    require(nls->count() == 0 && standard->count() == 0,
        "An empty shader family synthesized a profile row");

    requireControl<QPushButton>(window,
        QStringLiteral("config.shader.nls.add"))->click();
    require(nls->count() == 1 && nls->currentItem(),
        "NLS could not create its first ordinary profile");
    requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.nls.label"))->setText(
            QStringLiteral("Off"));
    require(nls->currentItem()->text() == QStringLiteral("Off"),
        "First NLS profile did not use its configured name");
    answerMessageBox(QMessageBox::Yes);
    requireControl<QPushButton>(window,
        QStringLiteral("config.shader.nls.remove"))->click();
    require(nls->count() == 0,
        "NLS could not remove its final configured profile");

    requireControl<QPushButton>(window,
        QStringLiteral("config.shader.standard.add"))->click();
    require(standard->count() == 1 && standard->currentItem(),
        "Standard could not create its first ordinary profile");
    requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.standard.label"))->setText(
            QStringLiteral("Off"));
    require(standard->currentItem()->text() == QStringLiteral("Off"),
        "First Standard profile did not use its configured name");
    answerMessageBox(QMessageBox::Yes);
    requireControl<QPushButton>(window,
        QStringLiteral("config.shader.standard.remove"))->click();
    require(standard->count() == 0,
        "Standard could not remove its final configured profile");

    save(window);
    const QByteArray saved = readBytes(path);
    require(!saved.contains("[shader.nls") &&
        !saved.contains("[shader.standard"),
        "Saving empty shader lists recreated a synthetic root section");
    ConfigEditorWindow reloaded(path, 0, true);
    require(requireControl<QListWidget>(reloaded,
        QStringLiteral("config.shader.nls.modes"))->count() == 0 &&
        requireControl<QListWidget>(reloaded,
            QStringLiteral("config.shader.standard.items"))->count() == 0,
        "Empty shader lists did not survive save and reload");
}

void testDisablingShaderRulePreservesShortcut()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    QListWidget* modes = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.nls.modes"));
    require(modes->count() > 1, "NLS fixture did not load");
    modes->setCurrentRow(1);

    QLineEdit* shortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.nls.shortcut"));
    QCheckBox* useRule = requireControl<QCheckBox>(window,
        QStringLiteral("config.shader.nls.use_rule"));
    QPlainTextEdit* rule = requireControl<QPlainTextEdit>(window,
        QStringLiteral("config.shader.nls.when"));
    shortcut->setText(QStringLiteral("Shift+N"));
    rule->setPlainText(QStringLiteral("${eotf} == \"HDR\""));
    require(useRule->isChecked(), "Entering a shader rule did not enable it");
    useRule->setChecked(false);
    save(window);

    const QByteArray saved = readBytes(path);
    const qsizetype start = saved.indexOf("[shader.nls.standard]");
    const qsizetype end = saved.indexOf("[shader.nls.protected]", start);
    require(start >= 0 && end > start, "Shader mode sections were not saved");
    const QByteArray standard = saved.mid(start, end - start);
    require(standard.contains("shortcut: Shift+N"),
        "Disabling a shader rule removed its manual shortcut");
    require(!standard.contains("when:"),
        "Disabling a shader rule did not remove only its when expression");
}

void testRendererProfileSectionsCollapseAndPersist()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(2);
    window.show();
    QCoreApplication::processEvents();

    QToolButton* toneMapping = requireControl<QToolButton>(window,
        QStringLiteral("rendererSection.toneMapping"));
    QToolButton* processing = requireControl<QToolButton>(window,
        QStringLiteral("rendererSection.processing"));
	QToolButton* lut = requireControl<QToolButton>(window,
		QStringLiteral("rendererSection.calibration"));
    QStackedWidget* pages = requireControl<QStackedWidget>(window,
        QStringLiteral("settingsPages"));
	require(!toneMapping->isChecked() && !processing->isChecked() &&
		lut->isChecked(),
        "A renderer section was not collapsed initially");
    require(processing->text() == QStringLiteral("Processing") &&
        !processing->text().contains(u'_'),
        "The Processing section exposes an internal identifier instead of a friendly heading");
    QWidget* toneMappingContent = requireControl<QWidget>(window,
        QStringLiteral("rendererSection.toneMapping.content"));
    require(toneMappingContent->findChild<QLineEdit*>(
        QStringLiteral("config.vprenderer.sdr_target_nits")) != nullptr &&
        toneMappingContent->findChild<QLineEdit*>(
            QStringLiteral("config.vprenderer.sdr_black_nits")) != nullptr,
        "Renderer luminance controls are not grouped under Tone mapping");
    require(pages->widget(2)->findChild<QToolButton*>(
        QStringLiteral("rendererSection.calibration")) == nullptr &&
        pages->widget(2)->findChild<QToolButton*>(
            QStringLiteral("rendererSection.sourceColor")) == nullptr,
        "Display calibration or Source transfer is still shown in Rendering");
    window.selectPage(16);
    QToolButton* calibration = requireControl<QToolButton>(window,
        QStringLiteral("rendererSection.calibration"));
    QToolButton* sourceColor = calibration;
    QWidget* calibrationContent = requireControl<QWidget>(window,
        QStringLiteral("rendererSection.calibration.content"));
    require(calibration->isChecked() && sourceColor->isChecked(),
        "Display calibration and SDR gamma processing should initially be expanded");
    require(sourceColor->text() == QStringLiteral("Display calibration"),
        "The SDR input-transfer controls are not grouped under Color Config Source transfer");
    require(calibrationContent->findChild<QLineEdit*>(
        QStringLiteral("config.vprenderer.color.sdr_target_nits")) == nullptr &&
        pages->widget(16)->findChild<QLineEdit*>(
            QStringLiteral("config.vprenderer.color.sdr_black_nits")) == nullptr,
        "Color Config incorrectly exposes renderer white/black nits");
    require(window.findChild<QComboBox*>(
        QStringLiteral("config.vprenderer.default_screen_profile")) == nullptr,
        "The obsolete legacy screen-profile selector is still exposed");
    require(window.findChild<QComboBox*>(
        QStringLiteral("config.vprenderer.deband")) == nullptr,
        "The overlapping legacy debanding toggle is still exposed");
    require(pages->widget(2)->findChild<QToolButton*>(
        QStringLiteral("rendererSection.outputExperiments")) == nullptr,
        "Output experiments are still owned by Rendering");
    window.selectPage(13);
    QToolButton* outputExperiments = requireControl<QToolButton>(window,
        QStringLiteral("rendererSection.outputExperiments"));
    require(!outputExperiments->isChecked(),
        "Output experiments were expanded initially");
    QToolButton* advancedOutput = requireControl<QToolButton>(window,
        QStringLiteral("rendererSection.advancedOutput"));
    require(advancedOutput->isChecked(),
        "Output transport should be discoverable initially");
    QCheckBox* vpOwnedPresenter = requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.diagnostic_vp_owned_dxgi_presenter"));
    require(requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.output_diagnostics")) &&
        requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.diagnostic_disable_shader_cache")) &&
        requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.diagnostic_disable_compute")) &&
        requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.diagnostic_force_8bit_sdr_swapchain")) &&
        vpOwnedPresenter &&
        requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.diagnostic_allow_limited_g22")),
        "Output experiment controls are missing from the editor");
    require(vpOwnedPresenter->accessibleName() ==
        QStringLiteral("Force VP-owned DXGI presenter (flip only, beta)"),
        "The VP-owned DXGI beta path is not labelled as a flip-only diagnostic");
    require(requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.report_bt2020_to_display")),
        "The Color Config display-reporting control is not included in Display calibration");
    require(requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_gamma")),
        "The Color Config display-transfer control is not included in Display calibration");
    require(requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_presentation")) &&
        requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_range")) &&
        requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_transport_gamma")),
        "The ordinary output controls are not included in Advanced output");
    require(requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.quality")),
        "Rendering quality is not available at the top of the renderer profile");
    QComboBox* quality = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.quality"));
    require(quality->currentData().toString() == QStringLiteral("high") &&
        quality->currentText() == QStringLiteral("High") &&
        qobject_cast<QListView*>(quality->view())->isRowHidden(0),
        "The root rendering-quality selector exposes a redundant Default choice");
    require(quality->itemData(1).toString() == QStringLiteral("high") &&
        quality->itemData(2).toString() == QStringLiteral("balanced") &&
        quality->itemData(3).toString() == QStringLiteral("fast"),
        "Rendering-quality choices are not ordered High, Balanced, Fast");
    QComboBox* upscaler = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.scaling.upscaler"));
    QComboBox* downscaler = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.scaling.downscaler"));
    for (const QString& value : { QStringLiteral("none"),
        QStringLiteral("nearest"), QStringLiteral("oversample"),
        QStringLiteral("gaussian"), QStringLiteral("catmull_rom"),
        QStringLiteral("lanczos"), QStringLiteral("ewa_lanczos4sharpest") })
        require(upscaler->findData(value) >= 0,
            "The upscaler selector is missing a supported libplacebo filter");
    for (const QString& value : { QStringLiteral("box"),
        QStringLiteral("hermite"),
        QStringLiteral("gaussian"), QStringLiteral("catmull_rom"),
        QStringLiteral("mitchell"), QStringLiteral("lanczos") })
        require(downscaler->findData(value) >= 0,
            "The downscaler selector is missing a supported libplacebo filter");
    require(upscaler->itemData(2).toString() ==
        QStringLiteral("ewa_lanczos4sharpest") &&
        upscaler->itemData(3).toString() == QStringLiteral("ewa_lanczossharp") &&
        upscaler->itemData(12).toString() == QStringLiteral("none") &&
        upscaler->itemText(12) == QStringLiteral("Built-in GPU sampling"),
		"Upscaler choices are not quality ordered or accurately labelled");
	require(downscaler->itemData(2).toString() == QStringLiteral("lanczos") &&
		downscaler->itemData(8).toString() == QStringLiteral("bilinear") &&
		downscaler->itemData(9).toString() == QStringLiteral("box") &&
		downscaler->itemData(10).toString() == QStringLiteral("gpu") &&
		downscaler->itemText(10) == QStringLiteral("Built-in GPU sampling") &&
		downscaler->findData(QStringLiteral("none")) < 0 &&
		downscaler->findData(QStringLiteral("ewa_lanczos")) < 0,
		"Downscaler choices include a removed or pathologically expensive mode");
    QComboBox* refreshRateSwitch = pages->widget(0)->findChild<QComboBox*>(
        QStringLiteral("config.general.switch_refresh_rate"));
    require(refreshRateSwitch != nullptr &&
        refreshRateSwitch->itemData(0).toString() == QStringLiteral("never") &&
        refreshRateSwitch->itemText(0) == QStringLiteral("Never") &&
        refreshRateSwitch->itemData(1).toString() == QStringLiteral("fullscreen_only") &&
        refreshRateSwitch->itemText(1) == QStringLiteral("Full Screen Only") &&
        refreshRateSwitch->itemData(2).toString() == QStringLiteral("always") &&
        refreshRateSwitch->itemText(2) == QStringLiteral("Always") &&
        pages->widget(2)->findChild<QComboBox*>(
        QStringLiteral("config.general.switch_refresh_rate")) == nullptr,
        "Switch refresh rate selector was not moved from Renderer Basic to General Display");
    QComboBox* debanding = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.deband_strength"));
    require(debanding->findData(QStringLiteral("AUTO")) >= 0 &&
        debanding->findData(QStringLiteral("off")) >= 0 &&
        debanding->findData(QStringLiteral("light")) >= 0 &&
        debanding->findData(QStringLiteral("default")) >= 0,
        "The canonical debanding selector is missing an expected value");
    require(debanding->itemData(1).toString() == QStringLiteral("AUTO") &&
        debanding->itemData(2).toString() == QStringLiteral("default") &&
        debanding->itemData(3).toString() == QStringLiteral("light") &&
        debanding->itemData(4).toString() == QStringLiteral("off"),
        "Debanding choices are not ordered Standard, Light, Off after Auto");
    QComboBox* sigmoid = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.scaling.sigmoid"));
    // The control was renamed "Sigmoid upscaling" (32bb858a); its accessible
    // name follows the visible label.
    require(sigmoid->accessibleName() == QStringLiteral("Sigmoid upscaling"),
        "Sigmoid scaling does not expose its accessible name");
    QComboBox* peakDetection = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.peak_detection"));
    require(peakDetection->itemData(1).toString() == QStringLiteral("AUTO") &&
        peakDetection->itemData(2).toString() == QStringLiteral("high_quality") &&
        peakDetection->itemData(3).toString() == QStringLiteral("on") &&
        peakDetection->itemData(4).toString() == QStringLiteral("off"),
        "Peak-detection choices are not ordered High quality, On, Off after Auto");
    QComboBox* dithering = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.dithering"));
    require(dithering->itemText(dithering->findData(QStringLiteral("AUTO"))) ==
        QStringLiteral("Auto") &&
        dithering->itemText(dithering->findData(QStringLiteral("blue_noise"))) ==
        QStringLiteral("Blue noise") &&
        dithering->itemText(dithering->findData(QStringLiteral("ordered_lut"))) ==
        QStringLiteral("Ordered (LUT)") &&
        dithering->itemText(dithering->findData(QStringLiteral("ordered_fixed"))) ==
        QStringLiteral("Ordered (fixed)") &&
        dithering->itemText(dithering->findData(QStringLiteral("white_noise"))) ==
        QStringLiteral("White noise") &&
        dithering->itemText(dithering->findData(
            QStringLiteral("error_diffusion_sierra_lite"))) ==
        QStringLiteral("Error diffusion: Sierra Lite") &&
        dithering->itemText(dithering->findData(
            QStringLiteral("error_diffusion_floyd_steinberg"))) ==
        QStringLiteral("Error diffusion: Floyd-Steinberg") &&
        dithering->itemText(dithering->findData(
            QStringLiteral("error_diffusion_sierra3"))) ==
        QStringLiteral("Error diffusion: Sierra 3") &&
        dithering->itemText(dithering->findData(QStringLiteral("off"))) ==
        QStringLiteral("Off"),
        "The dithering selector does not expose every libplacebo method");
    QComboBox* displayBitDepth = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.display_bit_depth"));
    require(displayBitDepth->property("effectiveValue").toString().compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0 &&
        displayBitDepth->currentText().startsWith(QStringLiteral("When unset:")),
        "Existing automatic dither depth was not preserved with its direct label");
    require(displayBitDepth->itemText(displayBitDepth->findData(QStringLiteral("8"))) == QStringLiteral("8-bit") &&
        displayBitDepth->itemText(displayBitDepth->findData(QStringLiteral("10"))) == QStringLiteral("10-bit"),
        "Explicit dither depth choices missing");

    require(window.findChild<QWidget*>(QStringLiteral("rendererSection.externalHdrLut.content")) == nullptr,
        "Separate LUT section remains after calibration grouping");
    require(calibrationContent->isVisibleTo(&window),
        "Display calibration content is visible while collapsed");

    window.selectPage(16);
    sourceColor->click();
    require(!sourceColor->isChecked(), "SDR gamma processing did not collapse");
    sourceColor->click();
    require(sourceColor->isChecked() && requireControl<QWidget>(window,
        QStringLiteral("rendererSection.calibration.content"))->isVisibleTo(&window),
        "Source transfer section did not expand");
    QListWidget* colorProfiles = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.color.profiles"));
    if (colorProfiles->count() > 1) colorProfiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(sourceColor->isChecked(),
        "Renderer section expansion state changed when selecting another profile");
    // Selecting the canonical control must retire the compatibility toggle,
    // so saving cannot leave two conflicting debanding values in one profile.
    window.selectPage(2);
    QListWidget* renderingProfiles = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.profiles"));
    renderingProfiles->setCurrentRow(0);
    QCoreApplication::processEvents();
    selectData(debanding, QStringLiteral("light"));
    selectData(displayBitDepth, QStringLiteral("8"));
    selectData(downscaler, QStringLiteral("gpu"));
    save(window);
    const QByteArray saved = readBytes(path);
    require(saved.contains("deband_strength: light") &&
        !saved.contains("\ndeband:") &&
        saved.contains("display_bit_depth: 8") &&
        saved.contains("downscaler: gpu"),
        "Renderer processing controls did not persist canonically");
}

void testOutputExperimentsPersistAndRestoreDefaults()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(13);
    window.show();
    QCoreApplication::processEvents();

    QToolButton* section = requireControl<QToolButton>(window,
        QStringLiteral("rendererSection.outputExperiments"));
    section->click();
    QComboBox* outputPathProfile = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_path_profile"));
    QComboBox* outputPresentation = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_presentation"));
    QComboBox* outputRange = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_range"));
    QComboBox* outputTransportGamma = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_transport_gamma"));
    QComboBox* outputGamma = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_gamma"));
    const QString originalPresentation = outputPresentation->currentData().toString();
    const QString originalRange = outputRange->currentData().toString();
    const QString originalGamma = outputGamma->currentData().toString();
    selectData(outputPathProfile, QStringLiteral("proposed"));
    require(outputPresentation->currentData().toString() == originalPresentation &&
        outputRange->currentData().toString() == originalRange &&
        outputGamma->currentData().toString() == originalGamma,
        "Diagnostic preset changed an ordinary output or calibration control");
    QCheckBox* limitedG22 = requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.diagnostic_allow_limited_g22"));
    QCheckBox* noCompute = requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.diagnostic_disable_compute"));
    QCheckBox* force8Bit = requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.diagnostic_force_8bit_sdr_swapchain"));
    QCheckBox* vpOwned = requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.color.diagnostic_vp_owned_dxgi_presenter"));
    selectData(outputPresentation, QStringLiteral("direct"));
    selectData(outputRange, QStringLiteral("limited"));
    selectData(outputTransportGamma, QStringLiteral("2.2"));
    require(outputPathProfile->currentData().toString() == QStringLiteral("proposed"),
        "Editing ordinary output or display calibration changed the diagnostic preset");
    require(limitedG22->isChecked() && !limitedG22->isEnabled(), "Limited2.2 did not derive its read-only flag");
    noCompute->setChecked(true);
    force8Bit->setChecked(true);
    vpOwned->setChecked(true);
    require(outputPathProfile->currentData().toString() == QStringLiteral("custom"),
        "Editing a proposed output-path value did not select Custom");
    save(window);
    const QByteArray configured = readBytes(path);
    require(configured.contains("diagnostic_allow_limited_g22: true") &&
        configured.contains("diagnostic_disable_compute: true") &&
        configured.contains("diagnostic_force_8bit_sdr_swapchain: true") &&
        configured.contains("diagnostic_vp_owned_dxgi_presenter: true") &&
        configured.contains("output_transport_gamma: 2.2") &&
        configured.contains("output_path_profile: custom"),
        "Output experiment controls did not persist with the renderer profile");

    answerMessageBox(QMessageBox::Yes);
    requireControl<QPushButton>(window,
        QStringLiteral("config.vprenderer.color.output_experiments.reset_defaults"))->click();
    require(limitedG22->isChecked() && !noCompute->isChecked() &&
        !force8Bit->isChecked() && !vpOwned->isChecked(),
        "Restore Normal Diagnostics did not reset the output experiment controls");
    require(outputPathProfile->currentData().toString() == QStringLiteral("legacy"),
        "Restore Normal Diagnostics did not select normal diagnostics");
    require(outputPresentation->currentData().toString().compare(
        QStringLiteral("direct"), Qt::CaseInsensitive) == 0 &&
        outputRange->currentData().toString().compare(
        QStringLiteral("limited"), Qt::CaseInsensitive) == 0 &&
        outputTransportGamma->currentData().toString() == QStringLiteral("2.2") &&
        outputGamma->currentData().toString() == originalGamma,
        "Restore Normal Diagnostics changed ordinary output or calibration controls");
    save(window);
    const QByteArray restored = readBytes(path);
    require(restored.contains("diagnostic_allow_limited_g22: true") &&
        restored.contains("diagnostic_disable_compute: false") &&
        restored.contains("diagnostic_force_8bit_sdr_swapchain: false") &&
        restored.contains("diagnostic_vp_owned_dxgi_presenter: false") &&
        restored.contains("output_path_profile: legacy") &&
        restored.toLower().contains("output_presentation: direct") &&
        restored.toLower().contains("output_range: limited") &&
        restored.contains("output_transport_gamma: 2.2"),
        "Restored normal diagnostics changed ordinary output or calibration settings");
}

void testEmptyShortcutsSurviveNlsBackendEdit()
{
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create temporary directory");
    const QString path = directory.filePath(QStringLiteral("VideoProcessor.cfg"));
    require(QFile::copy(repositoryPath(QStringLiteral("VideoProcessor.cfg")), path),
        "Cannot copy the current NLS configuration");
    QByteArray contents = readBytes(path);
    contents.replace("\r\n", "\n");
    const QByteArray shortcuts("[shortcuts]\n");
    const qsizetype position = contents.indexOf(shortcuts);
    require(position >= 0, "Fixture has no shortcuts section");
    contents.insert(position + shortcuts.size(),
        "render.2: Shift+M\n"
        "video_conversion_off: \n"
        "video_conversion_p010:\n"
        "auto_set: \n"
        "pq_set: \n"
        "capture_4: \n"
        "capture_3: \n"
        "capture_1: \n");
    QFile fixture(path);
    require(fixture.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Could not add empty shortcut fixture values");
    require(fixture.write(contents) == contents.size(),
        "Could not write empty shortcut fixture values");
    fixture.close();

    ConfigEditorWindow window(path, 0, true);
    QListWidget* modes = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.nls.modes"));
    QListWidgetItem* vertical = nullptr;
    for (int row = 0; row < modes->count(); ++row)
        if (modes->item(row)->data(Qt::UserRole).toString().compare(
            QStringLiteral("shader.nls.vertical"), Qt::CaseInsensitive) == 0)
            vertical = modes->item(row);
    require(vertical != nullptr, "NLS-V mode is missing");
    modes->setCurrentItem(vertical);
    QCoreApplication::processEvents();

    QLineEdit* hlsl = requireControl<QLineEdit>(window,
        QStringLiteral("config.shader.nls.hlsl_file"));
    hlsl->setText(QStringLiteral("NLS.hlsl"));
    QCoreApplication::processEvents();
    QPushButton* apply = requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration"));
    require(apply->isEnabled(),
        "An NLS edit was disabled by explicitly empty shortcuts");
    apply->click();
    QCoreApplication::processEvents();

    const QByteArray saved = readBytes(path);
    require(saved.contains("hlsl_file: NLS.hlsl"),
        "The NLS backend filename was not saved");
    require(saved.contains("video_conversion_off: \n") &&
        saved.contains("video_conversion_p010:\n") &&
        saved.contains("auto_set: \n") &&
        saved.contains("pq_set: \n") &&
        saved.contains("capture_4: \n") &&
        saved.contains("capture_3: \n") &&
        saved.contains("capture_1: \n"),
        "Explicitly disabled shortcuts were not preserved");
}

void testOutputDiagnosticPresetCanInherit()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(13);
    window.show();
    QCoreApplication::processEvents();

    requireControl<QPushButton>(window,
        QStringLiteral("config.vprenderer.color.add_profile"))->click();
    QCoreApplication::processEvents();
    QComboBox* outputPathProfile = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.output_path_profile"));
    selectData(outputPathProfile, QStringLiteral("proposed"));
    outputPathProfile->setCurrentIndex(0);
    save(window);

    const QByteArray configured = readBytes(path);
    require(!configured.contains("output_path_profile: \n") &&
        !configured.contains("output_path_profile:\n") &&
        !configured.contains("output_path_profile:\r\n"),
        "Selecting inherited diagnostics persisted an invalid empty output-path profile");
}

void testScreenConfigSectionsAndInlineUnits()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(4);
    window.show();
    QCoreApplication::processEvents();

    QToolButton* geometry = requireControl<QToolButton>(window,
        QStringLiteral("screenSection.geometry"));
    require(geometry->isChecked() &&
        geometry->text() == QStringLiteral("Screen geometry"),
        "Screen does not use the expected geometry section heading and state");
    require(requireControl<QWidget>(window,
        QStringLiteral("screenSection.geometry.content"))->isVisibleTo(&window) &&
        !window.findChild<QWidget*>(QStringLiteral("zoomSection.hdr_analysis.content")),
        "Screen geometry page contains Zoom content");
    require(window.findChild<QCheckBox*>(
        QStringLiteral("config.vprenderer.viewport.automatic_crop")) == nullptr,
        "Automatic black-bar crop was exposed as Screen geometry");

    window.selectPage(18);
    QCoreApplication::processEvents();
    QCheckBox* automaticCrop = requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.zoom.automatic_crop"));
    require(automaticCrop->accessibleName() ==
        QStringLiteral("Automatically crop black bars"),
        "Zoom does not expose the independent automatic black-bar crop option");
    require(!window.findChild<QWidget*>("config.vprenderer.zoom.hdr_peak_analysis_mode"),"HDR analysis must be moved out of Zoom");
    window.selectPage(19);QCoreApplication::processEvents();
    require(requireControl<QListWidget>(window,"config.vprenderer.subtitles.profiles")->count()==0,"Old Screen fixture unexpectedly has Subtitle profiles");
    requireControl<QPushButton>(window,"config.vprenderer.subtitles.add_profile")->click();
    QCoreApplication::processEvents();
    QToolButton* subtitles = requireControl<QToolButton>(window,
        QStringLiteral("subtitleSection.hdr_analysis"));
    require(!subtitles->isChecked() && subtitles->text() == QStringLiteral("HDR analysis"),
        "Zoom does not use the expected Subtitles section heading and state");
    subtitles->click();
    QCoreApplication::processEvents();
	QComboBox* hdrAnalysisMode = requireControl<QComboBox>(window,
		QStringLiteral(
			"config.vprenderer.subtitles.hdr_peak_analysis_mode"));
	require(hdrAnalysisMode->currentData().toString() == QStringLiteral("off") &&
		hdrAnalysisMode->accessibleName() ==
			QStringLiteral("HDR analysis protection") &&
		hdrAnalysisMode->count() == 3 &&
		hdrAnalysisMode->itemText(0) == QStringLiteral("Off") &&
		hdrAnalysisMode->itemData(0).toString() == QStringLiteral("off") &&
		hdrAnalysisMode->itemText(1) == QStringLiteral("Smart (Experimental)") &&
		hdrAnalysisMode->itemData(1).toString() == QStringLiteral("automatic") &&
		hdrAnalysisMode->itemText(2) == QStringLiteral("Percentage (Beta)") &&
		hdrAnalysisMode->itemData(2).toString() == QStringLiteral("fixed"),
		"Subtitle profiles do not expose the default-off exclusive HDR analysis modes");
	QLineEdit* hdrAnalysisHeight = requireControl<QLineEdit>(window,
		QStringLiteral(
			"config.vprenderer.subtitles.hdr_peak_analysis_height_percent"));
	require(hdrAnalysisHeight->text() == QStringLiteral("75") &&
		!hdrAnalysisHeight->isEnabled(),
		"HDR analysis height does not default to 75% or follow the disabled toggle");
	QComboBox* hdrAnalysisPosition = requireControl<QComboBox>(window,
		QStringLiteral("config.vprenderer.subtitles.hdr_peak_analysis_position"));
	require(hdrAnalysisPosition->currentData().toString() == QStringLiteral("top") &&
		!hdrAnalysisPosition->isEnabled(),
		"HDR analysis position does not default to disabled Top");
    require(!window.findChild<QCheckBox*>("config.vprenderer.zoom.subtitle_fit"),
        "Subtitle fitting still exposed on Zoom");
	selectData(hdrAnalysisMode, QStringLiteral("fixed"));
	require(hdrAnalysisHeight->isEnabled(),
		"HDR analysis height did not enable in fixed-percentage mode");
	require(hdrAnalysisPosition->isEnabled(),
		"HDR analysis position did not enable in fixed-percentage mode");
	hdrAnalysisHeight->setText(QStringLiteral("70"));
	selectData(hdrAnalysisPosition, QStringLiteral("bottom"));
	selectData(hdrAnalysisMode, QStringLiteral("automatic"));
	require(!hdrAnalysisHeight->isEnabled() && !hdrAnalysisPosition->isEnabled(),
		"HDR fixed-percentage controls remained enabled in automatic-movement mode");
    save(window);
    const QByteArray saved = readBytes(path);

	require(saved.contains("hdr_peak_analysis_picture_only: false"),
		"Automatic mode did not disable fixed-percentage HDR analysis");
	require(saved.contains("hdr_peak_analysis_motion_compensation: true"),
		"Automatic movement mode was not persisted in Subtitles");
	require(saved.contains("hdr_peak_analysis_height_percent: 70"),
		"HDR active-picture analysis height was not persisted in Subtitles");
	require(saved.contains("hdr_peak_analysis_position: bottom"),
		"HDR analysis position was not persisted in Subtitles");

    QListWidget* profiles = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.subtitles.profiles"));
    if (profiles->count() > 1) profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(subtitles->isChecked(),
        "Subtitle section expansion state changed when selecting another profile");
}

void testSeparatedProfilesDoNotLeaveShortcutShellsBehind()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("VideoProcessor.cfg"));
    const QByteArray configuration =
        "[vprenderer.rendering]\n"
        "quality: high\n\n"
        "[vprenderer.scaling_only]\n"
        "shortcut: Shift+1\n"
        "upscaler: ewa_lanczos\n"
        "downscaler: ewa_lanczos\n\n"
        "[vprenderer.color_only]\n"
        "shortcut: Shift+2\n"
        "sdr_target_primaries: REC709\n\n"
        "[vprenderer.output_only]\n"
        "shortcut: Shift+3\n"
        "output_presentation: DIRECT\n\n"
        "[vprenderer.viewport.scope]\n"
        "shortcut: F2\n"
        "screen_aspect: 2.35:1\n"
        "subtitle_fit: true\n\n"
        "[vprenderer.viewport.scope_and_crop]\n"
        "shortcut: Shift+2\n"
        "crop_narrower_content_to_fill_screen: true\n"
        "crop_narrower_content_aspect_limit: 2.20:1\n";
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        file.write(configuration) == configuration.size(),
        "Cannot create separated-profile migration fixture");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    const auto hasProfile = [](QListWidget* profiles, const QString& section)
    {
        for (int index = 0; index < profiles->count(); ++index)
            if (profiles->item(index)->data(Qt::UserRole).toString().compare(
                section, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    };
    QListWidget* rendering = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.profiles"));
    QListWidget* scaling = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.scaling.profiles"));
    QListWidget* color = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.color.profiles"));
    QListWidget* output = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.color.profiles"));
    QListWidget* screen = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.viewport.profiles"));
    QListWidget* zoom = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.zoom.profiles"));

    require(!hasProfile(rendering, QStringLiteral("vprenderer.scaling_only")) &&
        !hasProfile(rendering, QStringLiteral("vprenderer.color_only")) &&
        !hasProfile(rendering, QStringLiteral("vprenderer.output_only")),
        "A moved-only profile remained selectable in Rendering");
    require(hasProfile(scaling, QStringLiteral("vprenderer.scaling.scaling_only")) &&
        hasProfile(color, QStringLiteral("vprenderer.color.color_only")) &&
        !hasProfile(output, QStringLiteral("vprenderer.output.output_only")),
        "A separated owner did not receive its migrated profile");
    require(hasProfile(screen, QStringLiteral("vprenderer.viewport.scope")) &&
        !hasProfile(screen, QStringLiteral("vprenderer.viewport.scope_and_crop")) &&
        !hasProfile(zoom, QStringLiteral("vprenderer.zoom.scope")) &&
        hasProfile(zoom, QStringLiteral("vprenderer.zoom.scope_and_crop")),
        "Screen and Zoom migration did not separate their selectable profiles");

    save(window);
    const QByteArray saved = readBytes(path);
    require(!saved.contains("[vprenderer.scaling_only]") &&
        !saved.contains("[vprenderer.color_only]") &&
        !saved.contains("[vprenderer.output_only]") &&
        !saved.contains("[vprenderer.viewport.scope_and_crop]"),
        "A selector-only source section survived separated-profile migration");
    require(saved.contains("[vprenderer.scaling.scaling_only]") &&
        saved.contains("[vprenderer.color.color_only]") &&
        saved.contains("[legacy_output.output_only]") &&
        saved.contains("[vprenderer.zoom.scope_and_crop]"),
        "Separated profile settings were not persisted in their new owners");
    require(!saved.contains("downscaler: ewa_lanczos") &&
        !saved.contains("downscaler: none"),
        "A removed downscaler survived migration instead of becoming Auto");
}

void testQueueUnitsAndLutControlsUseConsistentRows()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(1);
    window.show();
    QCoreApplication::processEvents();

    QSpinBox* queueDepth = requireControl<QSpinBox>(window,
        QStringLiteral("config.queue.queue_size"));
    QLabel* queueUnit = requireControl<QLabel>(window,
        QStringLiteral("config.queue.queue_size.unit"));
    QLabel* recoveryUnit = requireControl<QLabel>(window,
        QStringLiteral("config.queue.reset_queue_too_large_percent.unit"));
    const int fixedUnitFieldWidth = requireControl<QLineEdit>(window,
        QStringLiteral("config.vprenderer.subtitles.offset_pixels"))->minimumWidth();
    const QStringList queueValueKeys = {
        QStringLiteral("queue_size"),
        QStringLiteral("lead_frames"),
        QStringLiteral("startup_preroll_frames"),
        QStringLiteral("target_frames"),
        QStringLiteral("target_latency_ms"),
        QStringLiteral("active_picture_lookahead_frames"),
        QStringLiteral("reset_after_render_restart_seconds"),
        QStringLiteral("reset_queue_too_large_percent")
    };
    for (const QString& key : queueValueKeys)
    {
        QSpinBox* value = requireControl<QSpinBox>(window,
            QStringLiteral("config.queue.") + key);
        require(value->minimumWidth() == fixedUnitFieldWidth &&
            value->maximumWidth() == fixedUnitFieldWidth &&
            value->alignment() == Qt::AlignRight,
            "Queue value controls do not use one consistent width and alignment");
    }
    require(queueDepth->suffix().isEmpty() &&
        queueUnit->text() == QStringLiteral("frames") &&
        recoveryUnit->text() == QStringLiteral("%"),
        "Queue units are still embedded in their value controls");
    require(queueUnit->x() >= queueDepth->x() + queueDepth->width(),
        "Queue unit label is not aligned beside its bounded value input");

    window.selectPage(16);
	QToolButton* lutSection = requireControl<QToolButton>(window,
		QStringLiteral("rendererSection.calibration"));
    if (!lutSection->isChecked()) lutSection->click();
    QCoreApplication::processEvents();
	QCheckBox* enabled = requireControl<QCheckBox>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut_enabled"));
	require(!enabled->isChecked(),
		"Display calibration LUT did not default to disabled");
	QComboBox* bt709 = requireControl<QComboBox>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut_bt709"));
	QComboBox* p3 = requireControl<QComboBox>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut_p3_d65"));
	QComboBox* bt2020 = requireControl<QComboBox>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut_bt2020"));
	require(!bt709->isEnabled() && !p3->isEnabled() && !bt2020->isEnabled(),
		"Calibration LUT selectors are interactive on first open while disabled");
	enabled->click();
	require(bt709->currentData().toString() == QStringLiteral("none") &&
		p3->currentData().toString() == QStringLiteral("none") &&
		bt2020->currentData().toString() == QStringLiteral("none") &&
		bt709->currentText() == QStringLiteral("None") &&
		p3->currentText() == QStringLiteral("None") &&
		bt2020->currentText() == QStringLiteral("None"),
		"Unused calibration LUT slots still expose a fabricated default");
    auto* gamut = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.target_primaries"));
    selectData(gamut, QStringLiteral("REC709"));
    for (int settle = 0; settle < 4; ++settle) QCoreApplication::processEvents();
    const int slotX = bt709->mapTo(&window, QPoint()).x();
    const int slotWidth = bt709->width();
    selectData(gamut, QStringLiteral("P3_D65"));
    QCoreApplication::processEvents();
    for (int settle = 0; settle < 4; ++settle) QCoreApplication::processEvents();
    require(p3->mapTo(&window, QPoint()).x() == slotX && p3->width() == slotWidth,
        "Switching LUT gamut changed the visible dropdown geometry");
	require(bt709->isEnabled() && p3->isEnabled() && bt2020->isEnabled(),
		"Calibration LUT controls did not enable with the calibration stage");
	require(requireControl<QComboBox>(window,
		QStringLiteral("config.vprenderer.tone_mapping"))->isEnabled() &&
		requireControl<QComboBox>(window,
		QStringLiteral("config.vprenderer.peak_detection"))->isEnabled(),
		"Enabling calibration incorrectly disabled DTM or peak analysis");
	require(window.findChild<QComboBox*>(
		QStringLiteral("config.vprenderer.lut")) == nullptr &&
		window.findChild<QToolButton*>(
			QStringLiteral("rendererSection.calibrationLut")) == nullptr,
		"Legacy calibration inspection controls remain on the editor surface");
	enabled->click();
	require(!bt2020->isEnabled(),
		"Disabling calibration did not gate its selectors");
}

void testActiveProfileMarkersCoverRelevantLists()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true);
    auto* queue = requireControl<QListWidget>(window, QStringLiteral("config.queue.profiles"));
    auto* renderer = requireControl<QListWidget>(window, QStringLiteral("config.vprenderer.profiles"));
    auto* scaling = requireControl<QListWidget>(window, QStringLiteral("config.vprenderer.scaling.profiles"));
    auto* color = requireControl<QListWidget>(window, QStringLiteral("config.vprenderer.color.profiles"));
    auto* output = requireControl<QListWidget>(window, QStringLiteral("config.vprenderer.color.profiles"));
    auto* viewport = requireControl<QListWidget>(window, QStringLiteral("config.vprenderer.viewport.profiles"));
    auto* shader = requireControl<QListWidget>(window, QStringLiteral("config.shader.nls.modes"));
    require(color->count() >= 2,
        "Color Config fixture does not expose distinct Rec.709 and BT.2020 profiles");
    window.setActiveProfileStatusForTesting(
        queue->item(0)->data(Qt::UserRole).toString(),
        renderer->item(0)->data(Qt::UserRole).toString(),
        color->item(0)->data(Qt::UserRole).toString(),
        viewport->item(0)->data(Qt::UserRole).toString(),
        { shader->item(0)->data(Qt::UserRole).toString() }, true, {},
        scaling->item(0)->data(Qt::UserRole).toString(),
        output->item(0)->data(Qt::UserRole).toString());
    require(queue->item(0)->data(Qt::UserRole + 12).toBool() &&
        renderer->item(0)->data(Qt::UserRole + 12).toBool() &&
        scaling->item(0)->data(Qt::UserRole + 12).toBool() &&
        color->item(0)->data(Qt::UserRole + 12).toBool() &&
        output->item(0)->data(Qt::UserRole + 12).toBool() &&
        viewport->item(0)->data(Qt::UserRole + 12).toBool() &&
        shader->item(0)->data(Qt::UserRole + 12).toBool(),
        "A resolved active profile did not receive its active marker");
    window.setActiveProfileStatusForTesting(
        queue->item(0)->data(Qt::UserRole).toString(),
        renderer->item(0)->data(Qt::UserRole).toString(),
        color->item(1)->data(Qt::UserRole).toString(),
        viewport->item(0)->data(Qt::UserRole).toString(),
        { shader->item(0)->data(Qt::UserRole).toString() });
    require(!color->item(0)->data(Qt::UserRole + 12).toBool() &&
        color->item(1)->data(Qt::UserRole + 12).toBool(),
        "The Color Config active marker did not follow a shortcut selection");

    window.selectPage(1);
    window.show();
    QCoreApplication::processEvents();
    queue->setCurrentRow(0);
    QCoreApplication::processEvents();
    const QRect row = queue->visualItemRect(queue->item(0));
    const QImage pixels = queue->viewport()->grab().toImage();
    const QColor dot = pixels.pixelColor(8, row.center().y());
    const QColor selectedBackground = pixels.pixelColor(2, row.center().y());
    require(dot.green() > dot.red() && dot.green() > dot.blue(),
        "The active marker was not painted as a green dot");
    require(selectedBackground.blue() > selectedBackground.red() &&
        selectedBackground.blue() > selectedBackground.green(),
        "The selected active row did not retain a full-width blue background");
}

void testActiveProfileOpensSelectedAfterLateStatusAndPreservesEdits()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true);
    auto* renderer = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.profiles"));
    auto* zoom = requireControl<QListWidget>(window,
        QStringLiteral("config.vprenderer.zoom.profiles"));
    auto* rendererStatus = requireControl<QLabel>(window,
        QStringLiteral("config.vprenderer.selected_profile_status"));
    auto* zoomStatus = requireControl<QLabel>(window,
        QStringLiteral("config.vprenderer.zoom.selected_profile_status"));
    require(renderer->count() >= 2 && zoom->count() >= 2,
        "Profile fixture needs a nondefault Rendering and Zoom profile");
    require(renderer->currentRow() == 0 && zoom->currentRow() == 0,
        "Profile lists did not start with an editing selection");

    // A snapshot may arrive after the editor has built its initial pages.
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, {}, true, {});
    require(renderer->currentRow() == 0 && zoom->currentRow() == 0,
        "An incomplete live snapshot moved the editing selection");
    const QString activeRenderer = renderer->item(1)->data(Qt::UserRole).toString();
    const QString activeZoom = zoom->item(1)->data(Qt::UserRole).toString();
    window.setActiveProfileStatusForTesting({}, activeRenderer, {}, {}, {},
        true, activeZoom);
    require(renderer->currentRow() == 1 && zoom->currentRow() == 1,
        "A late live snapshot did not select the active profiles");
    require(rendererStatus->text().endsWith(QStringLiteral("(Active)")) &&
        zoomStatus->text().endsWith(QStringLiteral("(Active)")),
        "The selected profile details did not show the live active state");

    renderer->setCurrentRow(0);
    zoom->setCurrentRow(0);
    window.setActiveProfileStatusForTesting({}, activeRenderer, {}, {}, {},
        true, activeZoom);
    require(renderer->currentRow() == 0 && zoom->currentRow() == 0,
        "A status refresh displaced the profile selected for editing");
    require(rendererStatus->text().endsWith(QStringLiteral("(Not active)")) &&
        zoomStatus->text().endsWith(QStringLiteral("(Not active)")),
        "The details label did not update after an editing selection");
    window.setActiveProfileStatusForTesting({},
        renderer->item(0)->data(Qt::UserRole).toString(), {}, {}, {}, true,
        zoom->item(0)->data(Qt::UserRole).toString());
    require(rendererStatus->text().endsWith(QStringLiteral("(Active)")) &&
        zoomStatus->text().endsWith(QStringLiteral("(Active)")),
        "A changed live profile did not refresh the selected details label");

    window.show();
    QCoreApplication::processEvents();
    window.hide();
    QCoreApplication::processEvents();
    window.show();
    QCoreApplication::processEvents();
    window.setActiveProfileStatusForTesting({}, activeRenderer, {}, {}, {},
        true, activeZoom);
    require(renderer->currentRow() == 1 && zoom->currentRow() == 1,
        "Reopening Config did not select the current active profiles");

    QTemporaryDir earlyDirectory;
    ConfigEditorWindow earlyWindow(copyFixture(earlyDirectory), 0, true);
    auto* earlyRenderer = requireControl<QListWidget>(earlyWindow,
        QStringLiteral("config.vprenderer.profiles"));
    auto* earlyZoom = requireControl<QListWidget>(earlyWindow,
        QStringLiteral("config.vprenderer.zoom.profiles"));
    earlyRenderer->setCurrentRow(1);
    earlyZoom->setCurrentRow(1);
    earlyWindow.setActiveProfileStatusForTesting({},
        earlyRenderer->item(0)->data(Qt::UserRole).toString(), {}, {}, {},
        true, earlyZoom->item(0)->data(Qt::UserRole).toString());
    require(earlyRenderer->currentRow() == 1 && earlyZoom->currentRow() == 1,
        "A delayed snapshot displaced a row already chosen for editing");
}

void testActiveShaderMarkersUseAuthoritativeSet()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true);
    auto* shader = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.nls.modes"));
    require(shader->count() >= 3,
        "Shader fixture does not expose a default profile and two shader members");

    shader->setCurrentRow(2);
    const QString defaultProfile = shader->item(0)->data(Qt::UserRole).toString();
    const QString first = shader->item(1)->data(Qt::UserRole).toString();
    const QString second = shader->item(2)->data(Qt::UserRole).toString();

    window.setActiveProfileStatusForTesting({}, {}, {}, {}, { defaultProfile });
    require(shader->currentRow() == 2 &&
        shader->item(0)->data(Qt::UserRole + 12).toBool() &&
        !shader->item(2)->data(Qt::UserRole + 12).toBool(),
        "Default-profile activity was confused with the selected editing row");

    window.setActiveProfileStatusForTesting({}, {}, {}, {}, { first });
    require(!shader->item(0)->data(Qt::UserRole + 12).toBool() &&
        shader->item(1)->data(Qt::UserRole + 12).toBool() &&
        shader->currentRow() == 2,
        "A single active shader did not move independently of selection");

    window.setActiveProfileStatusForTesting({}, {}, {}, {}, { first, second });
    require(shader->item(1)->data(Qt::UserRole + 12).toBool() &&
        shader->item(2)->data(Qt::UserRole + 12).toBool(),
        "Multiple authoritative active shaders were collapsed to one row");

    window.setActiveProfileStatusForTesting({}, {}, {}, {}, { first }, false);
    for (int index = 0; index < shader->count(); ++index)
        require(!shader->item(index)->data(Qt::UserRole + 12).toBool(),
            "Unavailable shader state retained a guessed active marker");
}

void testActiveShaderStatusRejectsStaleGeneration()
{
    ActiveProfileStatus::Snapshot snapshot{};
    snapshot.rendererGeneration = 9;
    snapshot.shaderGeneration = 8;
    snapshot.shaderAvailable = 1;
    snapshot.shaderCount = 1;
    require(!ActiveProfileStatus::ShaderSetIsCurrent(snapshot),
        "A stale shader set was accepted for a newer renderer generation");
    snapshot.shaderGeneration = snapshot.rendererGeneration;
    require(ActiveProfileStatus::ShaderSetIsCurrent(snapshot),
        "A generation-current authoritative shader set was rejected");
}

void testStandaloneConfigAcceptsLiveActiveProfileStatus()
{
    ActiveProfileStatus::Publish(GetCurrentProcessId(), 1,
        { { "queue", "low_latency" }, { "display", "rec709" },
            { "color", "bt2020" } },
        7, true, { "shader.nls.nonlinear_stretch",
            "shader.future.member" }, "PQ (ST2084)", "BT2020", "test output", true, true, 42, "C:/test/VideoProcessor.cfg");
    ActiveProfileStatus::Snapshot snapshot;
    require(ActiveProfileStatus::Read(0, snapshot),
        "Standalone Config did not accept the live VP active-profile status");
    require(ActiveProfileStatus::ShaderSetIsCurrent(snapshot) &&
        snapshot.shaderCount == 2 &&
        std::string(snapshot.color) == "vprenderer.color.bt2020" &&
        std::string(snapshot.shaders[0]) == "shader.nls.nonlinear_stretch" &&
        std::string(snapshot.shaders[1]) == "shader.future.member",
        "Live status did not preserve the authoritative shader set");
    require(snapshot.calibrationStatusAvailable != 0 && snapshot.calibrationLutAttached != 0 &&
        snapshot.calibrationConfigIdentity == 42 && std::string(snapshot.calibrationConfigPath) == "C:/test/VideoProcessor.cfg",
        "Live status did not preserve structured LUT evidence and config identity");
    require(!ActiveProfileStatus::Read(GetCurrentProcessId() + 1, snapshot),
        "Active-profile status was accepted for the wrong VP process");
}

void testLutSelectorDiscoversInstallationLutFiles()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    const QString lutDirectory = directory.filePath(QStringLiteral("luts"));
    require(QDir().mkpath(lutDirectory), "Cannot create the temporary LUT directory");
    QFile lut(lutDirectory + QStringLiteral("/Test-Calibration.cube"));
    require(lut.open(QIODevice::WriteOnly), "Cannot create the temporary LUT fixture");
    lut.write("TITLE \"Test LUT\"\nLUT_3D_SIZE 2\n");
    lut.close();
	QByteArray configuration = readBytes(path);
	require(configuration.contains("[vprenderer.color.rec709]\n"),
		"Could not locate the renderer profile fixture baseline");
	configuration.replace("[vprenderer.color.rec709]\n",
		"[vprenderer.color.rec709]\ncalibration_lut_bt2020: luts\\Test-Calibration.cube\n");
	QFile configurationFile(path);
	require(configurationFile.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
		configurationFile.write(configuration) == configuration.size(),
		"Could not write the Windows-path LUT fixture");
	configurationFile.close();

	ConfigEditorWindow window(path, 0, true);
	QComboBox* selector = requireControl<QComboBox>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut_bt2020"));
	require(selector->findData(QStringLiteral("luts/Test-Calibration.cube")) >= 0,
        "The LUT selector did not discover a .cube file from the installation LUT folder");
	require(selector->itemText(selector->findData(
		QStringLiteral("luts/Test-Calibration.cube"))) ==
		QStringLiteral("Test-Calibration"),
		"The LUT selector exposes its path or .cube extension");
	require(selector->currentData().toString() ==
		QStringLiteral("luts/Test-Calibration.cube"),
		"A valid hand-written Windows LUT path was not normalized and selected");
    require(requireControl<QPushButton>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut.open_folder"))->text() == QStringLiteral("Open LUT folder"),
        "The LUT folder action is missing");

	requireControl<QCheckBox>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut_enabled"))->click();
    selectData(selector, QStringLiteral("luts/Test-Calibration.cube"));
    save(window);
	const QByteArray configured = readBytes(path);
	require(configured.contains(
		"calibration_lut_enabled: true") && configured.contains(
		"calibration_lut_bt2020: luts/Test-Calibration.cube"),
        "The LUT selector did not persist a configuration-relative path");
	require(QFile::remove(lut.fileName()),
		"Could not remove the temporary LUT fixture");
	QFileSystemWatcher* watcher = window.findChild<QFileSystemWatcher*>();
	require(watcher != nullptr, "The calibration LUT directory watcher is missing");
	require(QMetaObject::invokeMethod(watcher, "directoryChanged",
		Qt::DirectConnection, Q_ARG(QString, lutDirectory)),
		"Could not refresh calibration LUT selectors after file removal");
	require(selector->currentData().toString() == QStringLiteral("luts/Test-Calibration.cube") &&
		selector->currentText().startsWith(QStringLiteral("Missing:")),
		"A removed LUT was not preserved as a missing reference");
	require(readBytes(path).contains("calibration_lut_bt2020:"),
		"File watcher removed the saved LUT path");
}

void testInheritedCalibrationLutUsesEffectiveControlState()
{
	QTemporaryDir directory;
	const QString path = copyFixture(directory);
	QByteArray configuration = readBytes(path);
	const QByteArray baseline =
		"[vprenderer.color.rec709]\n"
		"calibration_lut_enabled: true\n"
		"calibration_lut_bt2020: luts/Missing-Calibration.cube\n";
	require(configuration.contains("[vprenderer.color.rec709]\n"),
		"Could not locate inherited calibration profile fixture baseline");
	configuration.replace("[vprenderer.color.rec709]\n", baseline);
	QFile file(path);
	require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
		file.write(configuration) == configuration.size(),
		"Could not write inherited calibration profile fixture");
	file.close();

	ConfigEditorWindow window(path, 0, true);
	QListWidget* profiles = requireControl<QListWidget>(window,
		QStringLiteral("config.vprenderer.color.profiles"));
	require(profiles->count() >= 2,
		"Inherited calibration fixture has no named variant");
	profiles->setCurrentRow(1);
	QCoreApplication::processEvents();
	QCheckBox* enabled = requireControl<QCheckBox>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut_enabled"));
	QComboBox* slot = requireControl<QComboBox>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut_bt2020"));
	QComboBox* internalToneMap = requireControl<QComboBox>(window,
		QStringLiteral("config.vprenderer.tone_mapping"));
	require(enabled->isChecked(),
		"Named profile does not display its inherited calibration enable state");
	require(slot->isEnabled() && internalToneMap->isEnabled(),
		"Calibration inheritance incorrectly disables DTM controls");
	require(slot->currentData().toString().isEmpty() &&
		slot->currentText().contains(QStringLiteral("Missing:")),
		"An inherited missing LUT path was not preserved as a missing reference");
	save(window);
	require(readBytes(path).contains("calibration_lut_bt2020:"),
		"Saving removed the inherited stale LUT path");
}

void testCalibrationLayoutKeepsIndependentLutSlots()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    auto* method = requireControl<QRadioButton>(window, "config.vprenderer.color.calibration_method.lut");
    auto* enabled = requireControl<QCheckBox>(window, "config.vprenderer.color.calibration_lut_enabled");
    auto* gamut = requireControl<QComboBox>(window, "config.vprenderer.color.target_primaries");
    auto* rec = requireControl<QComboBox>(window, "config.vprenderer.color.calibration_lut_bt709");
    auto* p3 = requireControl<QComboBox>(window, "config.vprenderer.color.calibration_lut_p3_d65");
    auto* bt = requireControl<QComboBox>(window, "config.vprenderer.color.calibration_lut_bt2020");
    enabled->setChecked(true);
    QCoreApplication::processEvents();
    require(method->isChecked(), "Calibration method did not reflect profile enablement");
    gamut->setCurrentIndex(gamut->findData("REC709"));
    require(!rec->isHidden() && p3->isHidden() && bt->isHidden(), "LUT layout did not select Rec709 slot");
    const QVariant savedRec = rec->currentData();
    gamut->setCurrentIndex(gamut->findData("P3_D65"));
    require(rec->isHidden() && !p3->isHidden() && bt->isHidden(), "LUT layout did not select P3 slot");
    require(rec->currentData() == savedRec, "Changing visible gamut modified the other LUT slot");
    enabled->setChecked(false);
    require(!method->isChecked() && rec->isHidden() && p3->isHidden() && bt->isHidden(),
        "Display calibration mode left LUT selectors visible");
}

void testChoiceLabelsAndVpRendererName()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);

    QComboBox* nominalRange = requireControl<QComboBox>(window,
        QStringLiteral("config.directshow.renderer_nominal_range"));
    require(nominalRange->itemText(0) == QStringLiteral("Auto") &&
        nominalRange->itemData(0).toString() == QStringLiteral("AUTO"),
        "DirectShow Auto is not presented with normal title casing");
    require(nominalRange->findText(QStringLiteral("Default")) < 0,
        "DirectShow selector still contains a fabricated Default entry");

    QCheckBox* frameOffsetAuto = requireControl<QCheckBox>(window,
        QStringLiteral("config.directshow.frame_offset.auto"));
    QSpinBox* frameOffsetValue = requireControl<QSpinBox>(window,
        QStringLiteral("config.directshow.frame_offset.value"));
    require(!frameOffsetAuto->isTristate() && !frameOffsetAuto->isChecked() &&
        frameOffsetValue->isEnabled() && frameOffsetValue->value() == 90,
        "Fixed Frame offset is not presented as an editable numeric value");

    QComboBox* startStop = requireControl<QComboBox>(window,
        QStringLiteral("config.directshow.renderer_start_stop_time_method"));
    require(startStop->itemData(0).toString() == QStringLiteral("CLOCK_SMART") &&
        startStop->findText(QStringLiteral("Default")) < 0,
        "Start/stop selector does not use its first real mode as the fallback");

    QComboBox* peakDetection = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.peak_detection"));
    require(peakDetection->findText(QStringLiteral("Auto")) >= 0 &&
        peakDetection->findText(QStringLiteral("AUTO")) < 0 &&
        peakDetection->findText(QStringLiteral("Standard")) >= 0 &&
        peakDetection->findData(QStringLiteral("default")) < 0,
        "Renderer selector exposes duplicate or inconsistently cased automatic choices");

    QComboBox* quality = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.quality"));
    QComboBox* displayPrimaries = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.color.target_primaries"));
	require(displayPrimaries->count() == 3 &&
		displayPrimaries->currentData().toString() == QStringLiteral("REC709") &&
		displayPrimaries->findData(QStringLiteral("P3_D65")) >= 0 &&
        displayPrimaries->findText(QStringLiteral("Default"),
            Qt::MatchStartsWith) < 0,
        "Target gamut still exposes Default as a separate choice");
	require(requireControl<QLabel>(window,
		QStringLiteral("config.vprenderer.color.output_gamma.auto_status"))->text() ==
			QStringLiteral("Auto: Follows accepted transport; see live output"),
		"Output gamma Auto control does not identify its effective policy");
	auto* calibrationEnabled = requireControl<QCheckBox>(window,
		QStringLiteral("config.vprenderer.color.calibration_lut_enabled"));
	calibrationEnabled->setChecked(true);
	QCoreApplication::processEvents();
	require(requireControl<QLabel>(window,
		QStringLiteral("config.vprenderer.color.output_gamma.auto_status"))->text() ==
			QStringLiteral("Auto: Follows accepted transport; see live output"),
		"Calibration LUT enablement unexpectedly changed Auto gamma");
	calibrationEnabled->setChecked(false);
    require(requireControl<QCheckBox>(window, "config.vprenderer.color.sdr_adjust_gamma")->checkState() == Qt::Unchecked,
        "Saved automatic adjustment was not translated to passthrough");
    require(requireControl<QComboBox>(window, "config.vprenderer.color.sdr_input_transfer")->currentData() == "2.4",
        "Saved SDR automatic reference was not translated to 2.4");

	require(requireControl<QLabel>(window,
		QStringLiteral("config.vprenderer.display_bit_depth.auto_status"))->text() ==
			QStringLiteral("Auto: Output format"),
		"Display bit-depth Auto control does not identify its effective policy");
    QLineEdit* targetWhiteLevel = requireControl<QLineEdit>(window,
        QStringLiteral("config.vprenderer.sdr_target_nits"));
    QLineEdit* targetBlackLevel = requireControl<QLineEdit>(window,
        QStringLiteral("config.vprenderer.sdr_black_nits"));
    QLabel* targetBlackStatus = requireControl<QLabel>(window,
        QStringLiteral("config.vprenderer.sdr_black_nits.auto_status"));
    targetWhiteLevel->setText(QStringLiteral("79"));
    targetBlackLevel->setText(QStringLiteral("Auto"));
    QCoreApplication::processEvents();
    require(targetBlackStatus->text() == QStringLiteral("Auto: 0 nits (0.000001 nit internally)"),
        "Legacy Auto target black does not show the zero default and internal floor");
    QComboBox* upscaler = requireControl<QComboBox>(window,
        QStringLiteral("config.vprenderer.scaling.upscaler"));
    QLabel* upscalerStatus = requireControl<QLabel>(window,
        QStringLiteral("config.vprenderer.scaling.upscaler.auto_status"));
    for (const QString key : { QStringLiteral("upscaler"), QStringLiteral("downscaler"), QStringLiteral("sigmoid") })
    {
        auto* choice = requireControl<QComboBox>(window, "config.vprenderer.scaling." + key);
        require(qobject_cast<QListView*>(choice->view())->isRowHidden(0) &&
            !(choice->model()->flags(choice->model()->index(0, 0)) & Qt::ItemIsEnabled),
            "Default scaling profile still offers a redundant default choice");
        require(choice->currentData().toString() == "AUTO" && choice->currentText() == "Auto",
            "Omitted scaling setting does not display Auto");
    }
    require(upscalerStatus->text() == QStringLiteral("Auto: EWA Lanczos sharp") &&
        upscalerStatus->font().italic(),
        "Auto upscaler does not use a concise italic effective-value label");
    require(requireControl<QLabel>(window,
        QStringLiteral("config.vprenderer.dithering.auto_status"))->text() ==
            QStringLiteral("Auto: Blue noise"),
        "Auto dithering does not identify its blue-noise policy");
    selectData(quality, QStringLiteral("balanced"));
    require(upscalerStatus->text() == QStringLiteral("Auto: Lanczos"),
        "Auto policy preview did not refresh after rendering quality changed");
    selectData(upscaler, QStringLiteral("lanczos"));
    require(upscalerStatus->isHidden() && upscalerStatus->text().isEmpty(),
        "An Auto-policy preview remains visible for an explicit filter choice");
    selectData(upscaler, QStringLiteral("AUTO"));

    QComboBox* shaderStage = requireControl<QComboBox>(window,
        QStringLiteral("config.shader.nls.stage"));
    require(shaderStage->findText(QStringLiteral("Before resize")) >= 0 &&
        shaderStage->findData(QStringLiteral("pre_resize")) >= 0 &&
        shaderStage->findText(QStringLiteral("pre_resize")) < 0,
        "Shader stage exposes its raw config token");

    for (QComboBox* choice : window.findChildren<QComboBox*>())
        for (int index = 0; index < choice->count(); ++index)
        {
            const QString visible = choice->itemText(index);
            require(visible != QStringLiteral("AUTO"),
                "A selector still exposes AUTO instead of Auto");
            if (!choice->isEditable())
                require(!visible.contains(u'_'), QStringLiteral(
                    "A known selector exposes a raw underscored config token: %1 shows %2")
                    .arg(choice->objectName(), visible).toStdString().c_str());
        }
    for (QLineEdit* edit : window.findChildren<QLineEdit*>())
        require(edit->text() != QStringLiteral("AUTO"),
            "A text field still exposes AUTO instead of Auto");
    for (QCheckBox* check : window.findChildren<QCheckBox*>())
        require(!check->isTristate() || check->objectName() == "config.vprenderer.color.sdr_adjust_gamma",
            "An unrelated checkbox exposes an indeterminate third state");

    window.selectPage(4);
    QCheckBox* cropNarrower = requireControl<QCheckBox>(window,
        QStringLiteral("config.vprenderer.zoom.crop_narrower_content_to_fill_screen"));
    require(!cropNarrower->isTristate() &&
        cropNarrower->checkState() != Qt::PartiallyChecked,
        "A profile Boolean is exposed as a three-state checkbox");

    QComboBox* renderer = requireControl<QComboBox>(window,
        QStringLiteral("config.general.renderer"));
    frameOffsetAuto->setChecked(true);
    require(!frameOffsetValue->isEnabled(),
        "Automatic Frame offset did not disable the numeric value");
    selectData(renderer, QStringLiteral("VideoProcessor Renderer (Alpha)"));
    save(window);
    const QByteArray saved = readBytes(path);
    require(saved.contains("renderer: VP Renderer"),
        "Legacy Alpha renderer display name was not normalized when saved");
    require(saved.contains("frame_offset: AUTO") &&
        !saved.contains("frame_offset: Auto"),
        "Frame offset did not preserve the canonical AUTO token on disk");
}

void testLegacyRendererVisibilityDefaultsHiddenAndPreservesShortcuts()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);

    QCheckBox* hideLegacyRenderers = requireControl<QCheckBox>(window,
        QStringLiteral("config.general.hide_legacy_renderers"));
    require(hideLegacyRenderers->isChecked(),
        "Hide legacy renderers does not default to enabled when omitted");
    require(window.findChild<QLineEdit*>(
        QStringLiteral("config.shortcuts.render.1")) == nullptr,
        "A renderer shortcut was exposed without a visible discovered renderer");

    requireControl<QCheckBox>(window,
        QStringLiteral("config.general.fullscreen"))->setChecked(false);
    save(window);
    const QByteArray saved = readBytes(path);
    require(saved.contains("render.1: A") && saved.contains("render.2: M"),
        "Hidden renderer shortcuts were not preserved verbatim");
    require(!saved.contains("hide_legacy_renderers:"),
        "The editor wrote the unchanged default instead of preserving its omission");

    QTemporaryDir toggleDirectory;
    const QStringList filteredRenderers = {
        QStringLiteral("VP Renderer"), QStringLiteral("DirectShow - madVR") };
    QStringList allRenderers = {
        QStringLiteral("VP Renderer"), QStringLiteral("DirectShow - madVR"),
        QStringLiteral("Enhanced Video Renderer") };
    for (int index = 0; index < 128; ++index)
        allRenderers.push_back(QStringLiteral("Cached legacy renderer %1").arg(index));
    ConfigEditorWindow toggleWindow(copyFixture(toggleDirectory), 0, true,
        filteredRenderers, allRenderers);
    require(!QApplication::isEffectEnabled(Qt::UI_AnimateCombo),
        "Config left Qt's delayed combo-box animation enabled");
    QCheckBox* toggle = requireControl<QCheckBox>(toggleWindow,
        QStringLiteral("config.general.hide_legacy_renderers"));
    QComboBox* renderer = requireControl<QComboBox>(toggleWindow,
        QStringLiteral("config.general.renderer"));
    const int legacyIndex = renderer->findData(
        QStringLiteral("Enhanced Video Renderer"));
    auto* rendererList = qobject_cast<QListView*>(renderer->view());
    QLineEdit* legacyShortcut = requireControl<QLineEdit>(toggleWindow,
        QStringLiteral("config.shortcuts.render.3"));
    require(rendererList != nullptr && legacyIndex >= 0 &&
        legacyShortcut->isHidden(),
        "Renderer dropdown does not expose its cached list view");
    toggleWindow.show();
    for (QWidget* ancestor = renderer->parentWidget(); ancestor;
        ancestor = ancestor->parentWidget())
        if (auto* scrollArea = qobject_cast<QScrollArea*>(ancestor))
        {
            scrollArea->ensureWidgetVisible(renderer);
            break;
        }
    QCoreApplication::processEvents();
    require(renderer->isVisibleTo(&toggleWindow),
        "Renderer dropdown is not visible for the first-open latency test");
    QElapsedTimer firstOpenTimer;
    firstOpenTimer.start();
    renderer->showPopup();
    QCoreApplication::processEvents();
    const qint64 firstOpenElapsed = firstOpenTimer.elapsed();
    const std::string firstOpenFailure = QStringLiteral(
        "The cached renderer dropdown still performs deferred work on first open (%1 ms)")
        .arg(firstOpenElapsed).toStdString();
    require(firstOpenElapsed < 100,
        firstOpenFailure.c_str());
    renderer->hidePopup();
    const int cachedRendererCount = renderer->count();
    QElapsedTimer toggleTimer;
    toggleTimer.start();
    toggle->setChecked(false);
    require(toggleTimer.elapsed() < 100 &&
        !rendererList->isRowHidden(legacyIndex) &&
        !legacyShortcut->isHidden(),
        "Showing legacy renderers did not reveal cached UI rows");
    toggleTimer.restart();
    toggle->setChecked(true);
    require(toggleTimer.elapsed() < 100 &&
        rendererList->isRowHidden(legacyIndex) &&
        legacyShortcut->isHidden() &&
        renderer->count() == cachedRendererCount &&
        renderer->currentData().toString() == QStringLiteral("DirectShow - madVR"),
        "Hiding legacy renderers rebuilt the model or changed selection");
}

void testCustomShaderParametersExpandIntoPageScroll()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QByteArray configuration = readBytes(path);
    const qsizetype section = configuration.indexOf("[shader.nls.standard]\n");
    const qsizetype nextSection = configuration.indexOf("\n[", section + 1);
    require(section >= 0 && nextSection > section,
        "Standard shader section was not found in the fixture");
    QByteArray parameters;
    for (int index = 0; index < 24; ++index)
        parameters += "custom_parameter_" + QByteArray::number(index) + ": value\n";
    configuration.insert(nextSection + 1, parameters);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Cannot prepare long custom parameter fixture");
    require(file.write(configuration) == configuration.size(),
        "Cannot write long custom parameter fixture");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    window.resize(840, 500);
    window.show();
    QCoreApplication::processEvents();
    QListWidget* modes = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.nls.modes"));
    modes->setCurrentRow(1);
    QToolButton* parameterToggle = requireControl<QToolButton>(window,
        QStringLiteral("config.shader.nls.parameters_toggle"));
    parameterToggle->setChecked(true);
    QCoreApplication::processEvents();
    QTableWidget* table = requireControl<QTableWidget>(window,
        QStringLiteral("config.shader.nls.parameters"));
    require(table->rowCount() >= 24,
        "Long custom parameter list did not load");
    require(table->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff,
        "Custom parameter table retained an internal vertical scrollbar");
    int rowHeight = 0;
    for (int row = 0; row < table->rowCount(); ++row)
        rowHeight += table->rowHeight(row);
    require(table->height() == table->frameWidth() * 2 +
        table->horizontalHeader()->height() + rowHeight,
        "Custom parameter table did not expand to show every row");

    QScrollArea* pageScroll = nullptr;
    for (QWidget* ancestor = table->parentWidget(); ancestor;
        ancestor = ancestor->parentWidget())
        if (auto* scroll = qobject_cast<QScrollArea*>(ancestor))
        {
            pageScroll = scroll;
            break;
        }
    require(pageScroll && pageScroll->verticalScrollBar()->maximum() > 0,
        "Long custom parameter list did not overflow the containing page");
}

void testUnchangedActiveProfileStatusDoesNotInvalidateLists()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true);
    auto* shader = requireControl<QListWidget>(window,
        QStringLiteral("config.shader.nls.modes"));
    require(shader->count() >= 3,
        "Shader fixture does not expose enough active-profile rows");

    const QString first = shader->item(1)->data(Qt::UserRole).toString();
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, { first });
    int dataChanges = 0;
    QObject::connect(shader->model(), &QAbstractItemModel::dataChanged,
        [&dataChanges] { ++dataChanges; });

    window.setActiveProfileStatusForTesting({}, {}, {}, {}, { first });
    require(dataChanges == 0,
        "An unchanged active-profile snapshot invalidated the list model");

    const QString second = shader->item(2)->data(Qt::UserRole).toString();
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, { second });
    require(dataChanges > 0,
        "A changed active-profile snapshot did not update the list model");
}

void testLegacyRendererVisibilityFiltersExistingUiImmediately()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    // Commit any fixture migration first so this test observes only the
    // visibility preference's apply classification.
    if (QPushButton* baselineApply = requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration")); baselineApply->isEnabled())
    {
        baselineApply->click();
        QCoreApplication::processEvents();
    }
    window.setRendererDiscoveryForTesting({
        QStringLiteral("VP Renderer"),
        QStringLiteral("DirectShow - madVR"),
        QStringLiteral("DirectShow - Video Renderer"),
        QStringLiteral("DirectShow - Video Mixing Renderer 9") }, {
        QStringLiteral("VP Renderer"),
        QStringLiteral("DirectShow - madVR") });
    QStackedWidget* pages = requireControl<QStackedWidget>(window,
        QStringLiteral("settingsPages"));
    pages->setCurrentIndex(7);
    QComboBox* renderer = requireControl<QComboBox>(window,
        QStringLiteral("config.general.renderer"));
    QComboBox* actionRenderer = requireControl<QComboBox>(window,
        QStringLiteral("config.actions.renderer"));
    const auto visibleComboItems = [](QComboBox* combo)
    {
        auto* view = qobject_cast<QListView*>(combo->view());
        require(view != nullptr, "Renderer combo does not use a list view");
        int count = 0;
        for (int index = 0; index < combo->count(); ++index)
            if (!view->isRowHidden(index)) ++count;
        return count;
    };
    const auto visibleRendererShortcuts = [&window]
    {
        int count = 0;
        for (QLineEdit* edit : window.findChildren<QLineEdit*>())
            if (edit->objectName().startsWith(QStringLiteral("config.shortcuts.render.")) &&
                !edit->isHidden())
                ++count;
        return count;
    };
    const int hiddenGeneralCount = visibleComboItems(renderer);
    const int hiddenActionCount = visibleComboItems(actionRenderer);
    const int hiddenShortcutCount = visibleRendererShortcuts();
    require(hiddenGeneralCount < renderer->count(),
        "The fixture exposes no hidden legacy renderer rows");
    QCheckBox* hideLegacyRenderers = requireControl<QCheckBox>(window,
        QStringLiteral("config.general.hide_legacy_renderers"));
    QElapsedTimer timer;
    timer.start();
    hideLegacyRenderers->setChecked(false);
    QCoreApplication::processEvents();
    const qint64 elapsedMs = timer.elapsed();

    require(requireControl<QComboBox>(window,
        QStringLiteral("config.general.renderer")) == renderer,
        "Toggling legacy renderer visibility replaced the General selector");
    require(requireControl<QComboBox>(window,
        QStringLiteral("config.actions.renderer")) == actionRenderer,
        "Toggling legacy renderer visibility replaced the Actions selector");
    require(elapsedMs < 250,
        "Filtering the cached renderer UI took 250 ms or longer");
    require(!requireControl<QCheckBox>(window,
        QStringLiteral("config.general.hide_legacy_renderers"))->isChecked(),
        "Immediate renderer filtering lost the checkbox state");
    require(requireControl<QStackedWidget>(window,
        QStringLiteral("settingsPages"))->currentIndex() == 7,
        "Filtering renderer-dependent controls changed the active editor page");
    require(visibleComboItems(renderer) > hiddenGeneralCount,
        "General did not immediately expose the cached legacy renderer rows");
    require(visibleComboItems(actionRenderer) > hiddenActionCount,
        "Actions did not immediately expose the cached renderer rows");
    require(visibleRendererShortcuts() > hiddenShortcutCount,
        "Shortcuts did not immediately expose the cached renderer rows");
    require(!requireControl<QLabel>(window,
        QStringLiteral("configurationEffectSummary"))->text().startsWith(
            QStringLiteral("Restart renderer:")),
        "The UI-only visibility preference still requests a renderer restart");
    require(requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration"))->isEnabled(),
        "The immediately applied UI preference cannot be persisted");
}

void testDirectShowOnlyInputPersistsAndGeneralCanBeCleared()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    {
        ConfigEditorWindow window(path, 0, true);
        auto* general = requireControl<QComboBox>(window, QStringLiteral("config.general.video_conversion"));
        require(general->currentData().toString().isEmpty(), "DirectShow was promoted on load");
        selectData(requireControl<QComboBox>(window, QStringLiteral("config.directshow.video_conversion")), QStringLiteral("NONE"));
        selectData(general, QStringLiteral("V210_TO_P010"));
        requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"))->click();
        QCoreApplication::processEvents();
        selectData(general, QString());
        requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"))->click();
        QCoreApplication::processEvents();
    }
    ConfigEditorWindow reopened(path, 0, true);
    require(requireControl<QComboBox>(reopened, QStringLiteral("config.general.video_conversion"))->currentData().toString().isEmpty(), "General could not be cleared");
    require(requireControl<QComboBox>(reopened, QStringLiteral("config.directshow.video_conversion"))->currentData().toString() == QStringLiteral("NONE"), "DirectShow value moved on save/reopen");
    require(requireControl<QComboBox>(reopened, QStringLiteral("config.vprenderer.input_processing.video_conversion"))->currentData().toString().isEmpty(), "VP Renderer acquired a DirectShow override");
}

void testGeneralInputApplyPreservesBackendOverrides()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QFile file(path);
    require(file.open(QIODevice::Append),
        "Could not append the VP Renderer input override fixture");
    file.write("\n[vprenderer.input_processing]\nvideo_conversion: NONE\n");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    // Keep [general] without an explicit conversion default and save an
    // unrelated General edit. The two backend-specific values must remain
    // independent instead of being flattened into the shared default.
    QComboBox* switchRefreshRate = requireControl<QComboBox>(window,
        QStringLiteral("config.general.switch_refresh_rate"));
    const int always = switchRefreshRate->findData(QStringLiteral("always"));
    require(always >= 0, "The Always refresh-rate switching option is missing");
    switchRefreshRate->setCurrentIndex(always);
    const QString effectSummary = requireControl<QLabel>(window,
        QStringLiteral("configurationEffectSummary"))->text();
    const std::string effectFailure = QStringLiteral(
        "The legacy output migration is not labelled as a capture restart: %1")
        .arg(effectSummary).toStdString();
    require(effectSummary.startsWith(QStringLiteral("Restart capture:")),
        effectFailure.c_str());
    requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"))->click();
    QCoreApplication::processEvents();

    const QByteArray saved = readBytes(path);
    const auto section = [&saved](const QByteArray& name)
    {
        const int begin = saved.indexOf(QByteArray("[") + name + QByteArray("]"));
        const int end = begin < 0 ? -1 : saved.indexOf(QByteArray("\n["), begin + 1);
        return begin < 0 ? QByteArray() : saved.mid(begin,
            end < 0 ? -1 : end - begin);
    };
    require(!section("general").contains("video_conversion:"),
        "Applying an unrelated General change wrote a conversion default");
    require(section("directshow").contains("video_conversion: V210_TO_P010"),
        "Applying a General change flattened the DirectShow conversion override");
    require(section("vprenderer.input_processing").contains("video_conversion: NONE"),
        "Applying a General change flattened the VP Renderer conversion override");
}

void testNewActionStartsUnconfigured()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(7);
    window.show();
    QCoreApplication::processEvents();

    QStackedWidget* actionDetails = requireControl<QStackedWidget>(window,
        QStringLiteral("config.actions.details"));
    require(actionDetails->currentIndex() == 1,
        "A configured action did not show its editable details");

    QTimer::singleShot(0, []
    {
        for (QWidget* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QInputDialog*>(widget))
            {
                dialog->setTextValue(QStringLiteral("Test action"));
                dialog->accept();
                return;
            }
    });
    requireControl<QPushButton>(window, QStringLiteral("config.actions.add"))->click();
    QCoreApplication::processEvents();

    QListWidget* actions = requireControl<QListWidget>(window,
        QStringLiteral("config.actions.items"));
    require(actions->currentItem() && actions->currentItem()->text() ==
        QStringLiteral("Test action"), "New action was not selected");
    require(actionDetails->currentIndex() == 1,
        "The action editor did not appear after adding an action");
    require(!requireControl<QCheckBox>(window,
        QStringLiteral("config.actions.enabled"))->isChecked(),
        "New action was enabled before its draft was complete");
    QListWidget* events = requireControl<QListWidget>(window,
        QStringLiteral("config.actions.on"));
    for (int index = 0; index < events->count(); ++index)
        require(events->item(index)->checkState() == Qt::Unchecked,
            "New action fabricated an event selection");
    require(requireControl<QPlainTextEdit>(window,
        QStringLiteral("config.actions.when"))->toPlainText().isEmpty(),
        "New action fabricated a condition");
	requireControl<QLineEdit>(window,
		QStringLiteral("config.actions.coalesce_role"))->setText(
			QStringLiteral("test-state"));
	requireControl<QSpinBox>(window,
		QStringLiteral("config.actions.delay_seconds"))->setValue(2);

    events->item(0)->setCheckState(Qt::Checked);
    requireControl<QLineEdit>(window, QStringLiteral("config.actions.run"))
        ->setText(QStringLiteral("sd"));
    requireControl<QCheckBox>(window, QStringLiteral("config.actions.enabled"))
        ->setChecked(true);
    save(window);
    const QByteArray saved = readBytes(path);
    require(saved.contains("enabled: false") && saved.contains("run: sd") &&
		saved.contains("coalesce_role: test-state") &&
		saved.contains("delay_seconds: 2"),
        "Incomplete enabled action was not automatically saved as a disabled draft");
    require(!requireControl<QCheckBox>(window,
        QStringLiteral("config.actions.enabled"))->isChecked(),
        "Automatically drafted action still appeared enabled after saving");
}

// audit 2026-10-16: a failed save must not leave a drafted action disabled in
// the document while its Enabled box still shows checked. The next save then
// drafts it again (and unchecks the box) instead of silently saving it off.
void testFailedSaveRollsBackActionDraft()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.selectPage(7);
    window.show();
    QCoreApplication::processEvents();
    QTimer::singleShot(0, []
    {
        for (QWidget* widget : QApplication::topLevelWidgets())
            if (auto* dialog = qobject_cast<QInputDialog*>(widget))
            {
                dialog->setTextValue(QStringLiteral("Rollback action"));
                dialog->accept();
                return;
            }
    });
    requireControl<QPushButton>(window, QStringLiteral("config.actions.add"))->click();
    QCoreApplication::processEvents();
    QListWidget* events = requireControl<QListWidget>(window, QStringLiteral("config.actions.on"));
    events->item(0)->setCheckState(Qt::Checked);
    requireControl<QLineEdit>(window, QStringLiteral("config.actions.run"))->setText(QStringLiteral("sd"));
    QCheckBox* enabled = requireControl<QCheckBox>(window, QStringLiteral("config.actions.enabled"));
    enabled->setChecked(true);

    // A read-only file makes the atomic replace fail after drafting.
    require(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser),
        "Cannot make the configuration read-only");
    QPushButton* apply = requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"));
    require(apply->isEnabled(), "Apply was not enabled after the action edit");
    apply->click();
    QCoreApplication::processEvents();
    const QString failed = requireControl<QLabel>(window, QStringLiteral("configurationStatus"))->text();
    require(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ReadUser | QFileDevice::WriteUser), "Cannot make the configuration writable again");
    require(!failed.startsWith(QStringLiteral("Changes saved")),
        "Saving over a read-only configuration unexpectedly succeeded");
    require(enabled->isChecked(), "A failed save changed the action's Enabled box");
    require(!readBytes(path).contains("Rollback action"), "A failed save wrote the configuration");

    save(window);
    const QString status = requireControl<QLabel>(window, QStringLiteral("configurationStatus"))->text();
    require(status.contains(QStringLiteral("disabled draft")),
        "After a failed save the incomplete action was saved without being drafted again");
    require(!enabled->isChecked() && readBytes(path).contains("enabled: false"),
        "After a failed save the drafted action did not show as disabled");
}

// audit 2026-10-16: the legacy Config's fixed-shortcut validation in the
// editor: duplicates, invalid chords and profile shortcuts that take a fixed
// shortcut block Apply with a clear message and mark the field.
void testShortcutConflictsBlockApply()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.show();
    QCoreApplication::processEvents();
    QPushButton* apply = requireControl<QPushButton>(window, QStringLiteral("applyConfiguration"));
    QLabel* status = requireControl<QLabel>(window, QStringLiteral("configurationStatus"));
    QLineEdit* capture1 = requireControl<QLineEdit>(window, QStringLiteral("config.shortcuts.capture_1"));

    capture1->setText(QStringLiteral("Ctrl+2"));
    QCoreApplication::processEvents();
    require(!apply->isEnabled(), "A duplicate fixed shortcut left Apply enabled");
    require(status->text().contains(QStringLiteral("must be unique")),
        "A duplicate fixed shortcut did not say why Apply is blocked");
    require(capture1->property("invalid").toBool(), "A duplicate fixed shortcut was not marked");

    capture1->setText(QStringLiteral("Ctrl+"));
    QCoreApplication::processEvents();
    require(!apply->isEnabled(), "A shortcut that cannot be canonicalised left Apply enabled");
    require(status->text().contains(QStringLiteral("not a valid shortcut")),
        "An invalid shortcut did not say why Apply is blocked");

    capture1->setText(QStringLiteral("Ctrl+9"));
    QCoreApplication::processEvents();
    require(apply->isEnabled(), "A corrected shortcut did not re-enable Apply");
    require(!capture1->property("invalid").toBool(), "A corrected shortcut stayed marked");

    QLineEdit* queueShortcut = requireControl<QLineEdit>(window, QStringLiteral("config.queue.shortcut"));
    queueShortcut->setText(QStringLiteral("Ctrl+I"));
    QCoreApplication::processEvents();
    require(!apply->isEnabled(), "A profile shortcut taking a fixed shortcut left Apply enabled");
    require(status->text().contains(QStringLiteral("already the fixed shortcut")),
        "A profile shortcut clash did not say why Apply is blocked");
    // Second pass (audit 2026-10-16): the fixture's render.1 is A, and VP
    // pairs a renderer selection with a profile shortcut on purpose.
    queueShortcut->setText(QStringLiteral("A"));
    QCoreApplication::processEvents();
    require(apply->isEnabled(), "A profile shortcut paired with a render.N chord blocked Apply");
    // Typed in lower case: after the save the field shows the saved spelling.
    queueShortcut->setText(QStringLiteral("ctrl+alt+q"));
    QCoreApplication::processEvents();
    save(window);
    const QByteArray saved = readBytes(path);
    require(saved.contains("capture_1: Ctrl+9") && saved.contains("shortcut: Ctrl+Alt+Q"),
        "Corrected shortcuts were not saved");
    require(queueShortcut->text() == QStringLiteral("Ctrl+Alt+Q"),
        "The shortcut field did not show the saved canonical spelling");
}

void testEmptyActionsShowEmptyState()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("VideoProcessor.cfg"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Unable to create empty action fixture");
    file.write("[general]\n");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    window.selectPage(7);
    QCoreApplication::processEvents();
    requireControl<QListWidget>(window, QStringLiteral("config.actions.items"))->clearSelection();
    QStackedWidget* actionDetails = requireControl<QStackedWidget>(window,
        QStringLiteral("config.actions.details"));
    require(actionDetails->currentIndex() == 0,
        "An empty action list showed a disabled draft instead of the empty state");
}

void testMissingConfigurationCanBeCreatedFromEditor()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("VideoProcessor.cfg"));
    require(!QFileInfo::exists(path), "Missing-config fixture already exists");
    ConfigEditorWindow window(path, 0, true);
    window.show();
    QCoreApplication::processEvents();

    QPushButton* ok = requireControl<QPushButton>(window,
        QStringLiteral("okConfiguration"));
    QPushButton* apply = requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration"));
    QLabel* status = requireControl<QLabel>(window,
        QStringLiteral("configurationStatus"));
    require(ok->isEnabled() && apply->isEnabled(),
        "Missing configuration was not exposed as a creatable document");
    require(!status->text().contains(QStringLiteral("not loaded"),
        Qt::CaseInsensitive), "Missing configuration still reports a load failure");

    apply->click();
    QCoreApplication::processEvents();
    require(QFileInfo::exists(path),
        "Apply did not create the missing VideoProcessor.cfg");
    require(status->text().contains(QStringLiteral("created safely"),
        Qt::CaseInsensitive), "First save did not report configuration creation");
    require(!apply->isEnabled(),
        "First save left the newly created configuration dirty");
    ConfigFile parsed;
    require(parsed.Load(path.toStdString()), "Generated config could not be reloaded");
    std::string error;
    std::vector<std::string> warnings;
    require(RendererConfigView(parsed).Validate(error, warnings), "Generated renderer config is invalid");
    require(warnings.empty(), "Generated configuration warns about its own policy location");
}

void testApplyOkCancelContract()
{
    {
        QTemporaryDir directory;
        const QString path = copyFixture(directory);
        ConfigEditorWindow window(path, 0, true);
        window.show();
        QCoreApplication::processEvents();

        QPushButton* ok = requireControl<QPushButton>(window,
            QStringLiteral("okConfiguration"));
        QPushButton* cancel = requireControl<QPushButton>(window,
            QStringLiteral("cancelConfiguration"));
        QPushButton* apply = requireControl<QPushButton>(window,
            QStringLiteral("applyConfiguration"));
        QWidget* footer = requireControl<QWidget>(window, QStringLiteral("footer"));
        QList<QPushButton*> footerButtons;
        for (int index = 0; index < footer->layout()->count(); ++index)
            if (auto* button = qobject_cast<QPushButton*>(
                footer->layout()->itemAt(index)->widget()))
                footerButtons.push_back(button);
        require(footerButtons.size() == 3 && footerButtons[0] == ok &&
            footerButtons[1] == cancel && footerButtons[2] == apply,
            "The footer does not contain exactly OK, Cancel, Apply in order");
        require(window.findChild<QPushButton*>(QStringLiteral("reloadConfiguration")) == nullptr &&
            window.findChild<QPushButton*>(QStringLiteral("validateConfiguration")) == nullptr,
            "Reload or Validate is still exposed in the editor footer");
        // The deployed fixture intentionally exercises migrations; commit
        // those first so this test begins at its last successfully saved form.
        if (apply->isEnabled())
        {
            apply->click();
            QCoreApplication::processEvents();
        }
        require(!apply->isEnabled(), "Apply was enabled for a clean document");

        QCheckBox* fullscreen = requireControl<QCheckBox>(window,
            QStringLiteral("config.general.fullscreen"));
        fullscreen->setChecked(!fullscreen->isChecked());
        require(apply->isEnabled(), "Apply was not enabled by an edit");
        QLabel* effect = requireControl<QLabel>(window,
            QStringLiteral("configurationEffectSummary"));
        require(effect->text() == QStringLiteral(
            "Takes effect next start: Startup / input"),
            "The pending effect summary did not honestly classify the edit");

        apply->click();
        QCoreApplication::processEvents();
        require(window.isVisible(), "Apply closed the editor");
        require(!apply->isEnabled(), "Apply remained enabled after a successful save");
        require(effect->text() == QStringLiteral("No pending changes"),
            "The effect summary did not remain visible after Apply");
        require(requireControl<QLabel>(window,
            QStringLiteral("configurationStatus"))->text().contains(
                QStringLiteral("Takes effect when VideoProcessor next starts")),
            "VP-absent Apply did not report the next-start behavior");
        require(!requireControl<QLabel>(window,
            QStringLiteral("configurationStatus"))->text().contains(
                QStringLiteral("Backup:")),
            "Configuration save status still reports a backup path");

        // A clean OK closes without rewriting the file or creating another
        // backup/runtime notification.
        const QStringList backupsBefore = QDir(directory.path()).entryList(
            { QStringLiteral("VideoProcessor.cfg.backup-*") }, QDir::Files);
        ok->click();
        QCoreApplication::processEvents();
        require(!window.isVisible(), "Clean OK did not close the editor");
        require(QDir(directory.path()).entryList({ QStringLiteral("VideoProcessor.cfg.backup-*") },
            QDir::Files) == backupsBefore,
            "Clean OK performed an unnecessary save");
    }

    {
        QTemporaryDir directory;
        const QString path = copyFixture(directory);
        ConfigEditorWindow window(path, 0, true);
        window.show();
        QCoreApplication::processEvents();
        QPushButton* apply = requireControl<QPushButton>(window,
            QStringLiteral("applyConfiguration"));
        if (apply->isEnabled())
        {
            apply->click();
            QCoreApplication::processEvents();
        }
        const QByteArray original = readBytes(path);
        QCheckBox* fullscreen = requireControl<QCheckBox>(window,
            QStringLiteral("config.general.fullscreen"));
        const bool initial = fullscreen->isChecked();
        fullscreen->setChecked(!initial);
        requireControl<QPushButton>(window,
            QStringLiteral("cancelConfiguration"))->click();
        QCoreApplication::processEvents();
        require(!window.isVisible(), "Cancel did not close the editor");
        require(readBytes(path) == original, "Cancel changed the configuration file");
        window.show();
        QCoreApplication::processEvents();
        require(requireControl<QCheckBox>(window,
            QStringLiteral("config.general.fullscreen"))->isChecked() == initial,
            "Cancel retained the discarded working-copy value");
        require(!requireControl<QPushButton>(window,
            QStringLiteral("applyConfiguration"))->isEnabled(),
            "Cancel retained a dirty working copy");
    }

    {
        QTemporaryDir directory;
        const QString path = copyFixture(directory);
        ConfigEditorWindow window(path, 0, true);
        window.show();
        QCoreApplication::processEvents();
        QPushButton* apply = requireControl<QPushButton>(window,
            QStringLiteral("applyConfiguration"));
        if (apply->isEnabled())
        {
            apply->click();
            QCoreApplication::processEvents();
        }
        QCheckBox* fullscreen = requireControl<QCheckBox>(window,
            QStringLiteral("config.general.fullscreen"));
        fullscreen->setChecked(!fullscreen->isChecked());
        QFile external(path);
        require(external.open(QIODevice::Append), "Cannot create external-save conflict");
        external.write("\n# external edit\n");
        external.close();
        const QStringList backupsBefore = QDir(directory.path()).entryList(
            { QStringLiteral("VideoProcessor.cfg.backup-*") }, QDir::Files);
        requireControl<QPushButton>(window,
            QStringLiteral("okConfiguration"))->click();
        QCoreApplication::processEvents();
        require(!window.isVisible(), "OK did not close after overwriting an external edit");
        const QByteArray saved = readBytes(path);
        require(saved.contains("fullscreen: false") && !saved.contains("# external edit"),
            "Config did not replace the externally edited file with its validated document");
        const QStringList backupsAfter = QDir(directory.path()).entryList(
            { QStringLiteral("VideoProcessor.cfg.backup-*") }, QDir::Files);
        require(backupsAfter == backupsBefore,
            "Overwriting an external edit created a timestamped backup");
    }

    {
        QTemporaryDir directory;
        const QString path = copyFixture(directory);
        ConfigEditorWindow window(path, 0, true);
        window.show();
        QCoreApplication::processEvents();
        QPushButton* apply = requireControl<QPushButton>(window,
            QStringLiteral("applyConfiguration"));
        if (apply->isEnabled()) save(window);
        const QByteArray before = readBytes(path);
        QCheckBox* sceneDetection = requireControl<QCheckBox>(window,
            QStringLiteral("config.general.scene_detect"));
        sceneDetection->setChecked(!sceneDetection->isChecked());
        requireControl<QPushButton>(window,
            QStringLiteral("okConfiguration"))->click();
        QCoreApplication::processEvents();
        require(!window.isVisible() && readBytes(path) != before,
            "Dirty OK did not commit successfully before closing");
    }
}

void testDirectShowOnlyEffectDoesNotRestartAlpha()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    QByteArray contents = readBytes(path);
    contents.replace("renderer: DirectShow - madVR", "renderer: VP Renderer");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
        "Cannot create Alpha renderer fixture");
    require(file.write(contents) == contents.size(),
        "Cannot write Alpha renderer fixture");
    file.close();

    ConfigEditorWindow window(path, 0, true);
    QPushButton* apply = requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration"));
    if (apply->isEnabled())
    {
        apply->click();
        QCoreApplication::processEvents();
    }
    QSpinBox* offset = requireControl<QSpinBox>(window,
        QStringLiteral("config.directshow.frame_offset.value"));
    offset->setValue(offset->value() + 1);
    QLabel* effect = requireControl<QLabel>(window,
        QStringLiteral("configurationEffectSummary"));
    require(effect->text() == QStringLiteral("Takes effect next start: DirectShow"),
        "A DirectShow-only edit incorrectly promised to restart Alpha");
}

void testInvalidRendererIsRejectedContinuously()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.reveal();
    QCoreApplication::processEvents();

    QPushButton* apply = requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration"));
    if (apply->isEnabled())
    {
        apply->click();
        QCoreApplication::processEvents();
    }
    const QByteArray saved = readBytes(path);
    QPushButton* ok = requireControl<QPushButton>(window,
        QStringLiteral("okConfiguration"));
    QComboBox* renderer = requireControl<QComboBox>(window,
        QStringLiteral("config.general.renderer"));
    QLabel* status = requireControl<QLabel>(window,
        QStringLiteral("configurationStatus"));

    require(!renderer->isEditable(),
        "Renderer discovery selector still accepts arbitrary typed values");
    renderer->addItem(QStringLiteral("DirectShow - madVR1a"),
        QStringLiteral("DirectShow - madVR1a"));
    selectData(renderer, QStringLiteral("DirectShow - madVR1a"));
    QCoreApplication::processEvents();
    require(status->text().contains(QStringLiteral("renderer 'DirectShow - madVR1a' was not discovered")) &&
        status->styleSheet().contains(QStringLiteral("#ff8d86")),
        "An invalid renderer did not immediately show the first validation error in red");
    require(!ok->isEnabled() && !apply->isEnabled(),
        "OK or Apply remained enabled for an invalid renderer");

    ok->click();
    apply->click();
    QCoreApplication::processEvents();
    require(window.isVisible() && readBytes(path) == saved,
        "An invalid renderer candidate was saved or closed the editor");

    selectData(renderer, QStringLiteral("VP Renderer"));
    require(ok->isEnabled() && apply->isEnabled(),
        "Correcting the renderer did not restore OK and Apply");
    require(!status->styleSheet().contains(QStringLiteral("#ff8d86")),
        "Correcting the renderer did not clear the validation error styling");
}

void testRendererDiscoveryOutageDoesNotBlockProfileEdits()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    // Production mode is important: test mode previously supplied a fallback
    // list and consequently masked this regression.
    ConfigEditorWindow window(path, 0, false);
    window.setRendererDiscoveryForTesting({}, {});
    QCoreApplication::processEvents();

    QPushButton* apply = requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration"));
    QLineEdit* shortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.vprenderer.viewport.shortcut"));
    shortcut->setText(QStringLiteral("Ctrl+F8"));
    QCoreApplication::processEvents();

    require(apply->isEnabled(),
        "An empty renderer discovery result blocked an otherwise valid profile edit");
}

void testShortcutEffectsAreClassifiedLive()
{
    {
        QTemporaryDir directory;
        const QString path = copyFixture(directory);
        ConfigEditorWindow window(path, 0, true);
        QPushButton* apply = requireControl<QPushButton>(window,
            QStringLiteral("applyConfiguration"));
        if (apply->isEnabled()) save(window);

        requireControl<QLineEdit>(window,
            QStringLiteral("config.shortcuts.fullscreen_toggle"))
            ->setText(QStringLiteral("Ctrl+Alt+F11"));
        QLabel* effect = requireControl<QLabel>(window,
            QStringLiteral("configurationEffectSummary"));
        require(effect->text() == QStringLiteral("Apply shortcuts live: Shortcuts"),
            "A shortcut-only edit was not classified for live shortcut replacement");

        QSpinBox* queueSize = requireControl<QSpinBox>(window,
            QStringLiteral("config.queue.queue_size"));
        queueSize->setValue(queueSize->value() + 1);
        require(effect->text().startsWith(QStringLiteral("Reset queues:")) &&
            effect->text().contains(QStringLiteral("Queue")) &&
            effect->text().contains(QStringLiteral("Shortcuts")),
            "A mixed queue and shortcut edit did not show the strongest action");
    }

    {
        QTemporaryDir directory;
        const QString path = copyFixture(directory);
        ConfigEditorWindow window(path, 0, true);
        QPushButton* apply = requireControl<QPushButton>(window,
            QStringLiteral("applyConfiguration"));
        if (apply->isEnabled()) save(window);

        requireControl<QLineEdit>(window, QStringLiteral("config.queue.shortcut"))
            ->setText(QStringLiteral("Ctrl+Alt+F10"));
        require(requireControl<QLabel>(window,
            QStringLiteral("configurationEffectSummary"))->text() ==
                QStringLiteral("Apply shortcuts live: Queue"),
            "A queue-profile shortcut-only edit was misclassified as a queue reset");
    }
}

void testDisplayWarningAndErrorPrecedence()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.reveal();
    QCoreApplication::processEvents();

    QLabel* status = requireControl<QLabel>(window,
        QStringLiteral("configurationStatus"));
    QPushButton* ok = requireControl<QPushButton>(window,
        QStringLiteral("okConfiguration"));
    QPushButton* apply = requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration"));
    require(status->text() == QStringLiteral("Warning: Display 'EPSON PJ' is not currently discoverable.") &&
        status->styleSheet().contains(QStringLiteral("#e0b45c")) && ok->isEnabled(),
        "An undiscovered display was not presented as a non-blocking advisory warning");

    QComboBox* renderer = requireControl<QComboBox>(window,
        QStringLiteral("config.general.renderer"));
    renderer->addItem(QStringLiteral("DirectShow - invalid"),
        QStringLiteral("DirectShow - invalid"));
    selectData(renderer, QStringLiteral("DirectShow - invalid"));
    require(status->text().contains(QStringLiteral("renderer 'DirectShow - invalid'")) &&
        !status->text().contains(QStringLiteral("Warning: Display")) &&
        status->styleSheet().contains(QStringLiteral("#ff8d86")) &&
        !ok->isEnabled() && !apply->isEnabled(),
        "A validation error did not override the display warning");

    selectData(renderer, QStringLiteral("DirectShow - madVR"));
    require(status->text().contains(QStringLiteral("Warning: Display 'EPSON PJ'")) &&
        status->styleSheet().contains(QStringLiteral("#e0b45c")) &&
        ok->isEnabled(),
        "Fixing all errors did not resurface the advisory display warning");
}

void testVideoOnlyStartupDefaultRoundTrips()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    QPushButton* apply = requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration"));
    if (apply->isEnabled()) save(window);

    QCheckBox* fullscreen = requireControl<QCheckBox>(window,
        QStringLiteral("config.general.fullscreen"));
    QCheckBox* videoOnly = requireControl<QCheckBox>(window,
        QStringLiteral("config.general.noui"));
    require(videoOnly->text() == QStringLiteral("Start as Video Only") &&
        videoOnly->parentWidget() == fullscreen->parentWidget(),
        "Start as Video Only is missing from the startup presentation controls");

    videoOnly->setChecked(true);
    QLabel* effect = requireControl<QLabel>(window,
        QStringLiteral("configurationEffectSummary"));
    require(effect->text() == QStringLiteral(
        "Takes effect next start: Startup / input"),
        "Video Only did not report next-start-only behavior");
    save(window);
    require(readBytes(path).contains("noui: true"),
        "Video Only did not persist the canonical [general] noui key");

    ConfigEditorWindow reopened(path, 0, true);
    require(requireControl<QCheckBox>(reopened,
        QStringLiteral("config.general.noui"))->isChecked(),
        "Video Only did not round-trip through a reopened editor");
}

void testRemainingEffectSummaryPrecedence()
{
    {
        QTemporaryDir directory;
        const QString path = copyFixture(directory);
        ConfigEditorWindow window(path, 0, true);
        QPushButton* apply = requireControl<QPushButton>(window,
            QStringLiteral("applyConfiguration"));
        if (apply->isEnabled()) save(window);
        QLabel* effect = requireControl<QLabel>(window,
            QStringLiteral("configurationEffectSummary"));

        QSpinBox* queueSize = requireControl<QSpinBox>(window,
            QStringLiteral("config.queue.queue_size"));
        const int originalQueueSize = queueSize->value();
        queueSize->setValue(originalQueueSize + 1);
        require(effect->text() == QStringLiteral("Reset queues: Queue"),
            "A queue-only edit did not report a queue reset");
        queueSize->setValue(originalQueueSize);
        require(effect->text() == QStringLiteral("No pending changes") &&
            !apply->isEnabled(),
            "Reverting a queue edit did not clear its pending effect");

        QCheckBox* logging = requireControl<QCheckBox>(window,
            QStringLiteral("config.logging.enabled"));
        logging->setChecked(!logging->isChecked());
        require(effect->text() == QStringLiteral(
            "Takes effect next start: Logging"),
            "A logging-only edit did not report next-start behavior");
    }

    {
        QTemporaryDir directory;
        const QString path = copyFixture(directory);
        ConfigEditorWindow window(path, 0, true);
        QPushButton* apply = requireControl<QPushButton>(window,
            QStringLiteral("applyConfiguration"));
        if (apply->isEnabled()) save(window);
        requireControl<QLineEdit>(window,
            QStringLiteral("config.shortcuts.fullscreen_toggle"))
            ->setText(QStringLiteral("Ctrl+Alt+F11"));
        selectData(requireControl<QComboBox>(window,
            QStringLiteral("config.general.renderer")),
            QStringLiteral("VP Renderer"));
        QLabel* effect = requireControl<QLabel>(window,
            QStringLiteral("configurationEffectSummary"));
        require(effect->text().startsWith(QStringLiteral("Restart renderer:")) &&
            effect->text().contains(QStringLiteral("Shortcuts")) &&
            effect->text().contains(QStringLiteral("Startup / input")),
            "A renderer plus shortcut edit did not report the strongest restart action");
    }
}

QString accessibleName(QWidget* widget)
{
    QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(widget);
    return interface ? interface->text(QAccessible::Name).trimmed() : QString();
}

QShortcut* shortcutFor(ConfigEditorWindow& window, const QKeySequence& sequence)
{
    for (QShortcut* shortcut : window.findChildren<QShortcut*>())
        if (shortcut->key() == sequence) return shortcut;
    return nullptr;
}

void testDpiKeyboardAndAccessibilityBehavior()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.show();
    window.resize(1040, 700);
    QCoreApplication::processEvents();

    require(window.minimumWidth() == 1040 && window.minimumHeight() == 700,
        "Minimum window size no longer preserves the usable dashboard layout");
    QStackedWidget* pages = requireControl<QStackedWidget>(window,
        QStringLiteral("settingsPages"));
    require(pages->currentIndex() == 0, "General was not the initial page");

    QShortcut* next = shortcutFor(window, QKeySequence(QStringLiteral("Ctrl+Tab")));
    QShortcut* previous = shortcutFor(window, QKeySequence(QStringLiteral("Ctrl+Shift+Tab")));
    QShortcut* saveKey = shortcutFor(window, QKeySequence::Save);
    require(next && previous && saveKey, "Required keyboard shortcuts are missing");
    QMetaObject::invokeMethod(next, "activated", Qt::DirectConnection);
    require(pages->currentIndex() == 1, "Ctrl+Tab did not select the next settings page");
    QMetaObject::invokeMethod(previous, "activated", Qt::DirectConnection);
    require(pages->currentIndex() == 0, "Ctrl+Shift+Tab did not select the previous settings page");

    QComboBox* capture = requireControl<QComboBox>(window,
        QStringLiteral("config.general.capture_device"));
    require((capture->focusPolicy() & Qt::TabFocus) != 0,
        "A primary field cannot receive keyboard focus");
    require(!accessibleName(capture).isEmpty(),
        "Capture device field has no accessible name");
    require(!accessibleName(requireControl<QLabel>(window,
        QStringLiteral("configurationStatus"))).isEmpty(),
        "Configuration status has no accessible name");

    window.selectPage(1);
    QCoreApplication::processEvents();
    QListWidget* queueProfiles = requireControl<QListWidget>(window,
        QStringLiteral("config.queue.profiles"));
    require(!accessibleName(queueProfiles).isEmpty(),
        "Queue profile list has no accessible name");
    QSplitter* queueSplitter = nullptr;
    for (QSplitter* splitter : window.findChildren<QSplitter*>())
        if (splitter->isVisible()) { queueSplitter = splitter; break; }
    require(queueSplitter && queueSplitter->orientation() == Qt::Horizontal,
        "Profile layout did not remain side-by-side at the supported minimum size");

    window.resize(1120, 760);
    QCoreApplication::processEvents();
    require(queueSplitter->orientation() == Qt::Horizontal,
        "Profile layout did not return to side-by-side at normal width");

    for (int page = 0; page < pages->count(); ++page)
    {
        window.selectPage(page);
        QCoreApplication::processEvents();
        for (QWidget* control : pages->currentWidget()->findChildren<QWidget*>())
        {
            if (!control->isVisibleTo(&window) || !control->isEnabled() ||
                (control->focusPolicy() & Qt::TabFocus) == 0 ||
                !control->objectName().startsWith(QStringLiteral("config.")))
                continue;
            if (accessibleName(control).isEmpty())
                throw std::runtime_error(QStringLiteral("Focusable control has no accessible name: %1")
                    .arg(control->objectName()).toStdString());
        }
    }
}

void testPrimaryDiscoverySelectorsOpenOnFieldClick()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    window.show();
    window.selectPage(0);
    QCoreApplication::processEvents();

    for (const QString& name : {
        QStringLiteral("config.general.capture_device"),
        QStringLiteral("config.general.fullscreen_monitor_name"),
        QStringLiteral("config.general.renderer") })
    {
        QComboBox* selector = requireControl<QComboBox>(window, name);
        require(!selector->isEditable(),
            "A discovery selector still behaves like a text editor");
        require(selector->lineEdit() == nullptr,
            "A discovery selector still owns an embedded text editor");
        require(selector->isVisibleTo(&window) && selector->isEnabled(),
            "A discovery selector is not available for direct interaction");

        const QPoint point = selector->rect().center();
        const QPoint globalPoint = selector->mapToGlobal(point);
        const ULONGLONG started = GetTickCount64();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(point),
            QPointF(globalPoint), Qt::LeftButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(selector, &press);
        QCoreApplication::processEvents();
        QWidget* popup = selector->view()->window();
        const ULONGLONG elapsed = GetTickCount64() - started;
        require(popup && popup->isVisible(),
            "Clicking the discovery selector field did not open its popup");
        require(elapsed < 250,
            "Discovery selector popup did not open promptly");
        selector->hidePopup();
        QCoreApplication::processEvents();
    }
}

void testOrdinaryEditsDoNotRequireDiskValidation()
{
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create validation test directory");
    const QString candidateDirectory =
        directory.filePath(QStringLiteral("removed-before-edit"));
    require(QDir().mkpath(candidateDirectory),
        "Cannot create validation candidate directory");
    const QString path = candidateDirectory +
        QStringLiteral("/VideoProcessor.cfg");
    require(QFile::copy(repositoryPath(QStringLiteral(
        "test-fixtures/deployed-VideoProcessor-20260807-current.cfg")), path),
        "Cannot copy validation candidate fixture");

    ConfigEditorWindow window(path, 0, true);
    const QString parked = directory.filePath(QStringLiteral("parked.cfg"));
    require(QFile::rename(path, parked) &&
        QDir().rmdir(candidateDirectory),
        "Cannot remove the candidate directory after loading");

    QCheckBox* fullscreen = requireControl<QCheckBox>(window,
        QStringLiteral("config.general.fullscreen"));
    fullscreen->setChecked(!fullscreen->isChecked());
    QCoreApplication::processEvents();
    require(requireControl<QPushButton>(window,
        QStringLiteral("applyConfiguration"))->isEnabled(),
        "An ordinary edit synchronously required a disk validation copy");
    require(!requireControl<QLabel>(window,
        QStringLiteral("configurationStatus"))->text().contains(
            QStringLiteral("validation copy"), Qt::CaseInsensitive),
        "Interactive validation attempted disk-backed core validation");
}

void testWarmRevealPathGrantsForegroundPermission()
{
    const QByteArray source = readBytes(repositoryPath(QStringLiteral(
        "src/VideoProcessor-GUI/VideoProcessorDlg.cpp")));
    const qsizetype functionStart = source.indexOf(
        "bool SignalConfigurationEditorReveal(DWORD processId)");
    const qsizetype functionEnd = source.indexOf(
        "bool SendConfigurationEditorRevealOnce", functionStart);
    require(functionStart >= 0 && functionEnd > functionStart,
        "Cannot locate the stable Config reveal path");
    const QByteArray revealPath = source.mid(functionStart,
        functionEnd - functionStart);
    const qsizetype grant = revealPath.indexOf(
        "AllowSetForegroundWindow(processId)");
    const qsizetype signal = revealPath.indexOf("SetEvent(eventHandle)");
    require(grant >= 0 && signal > grant,
        "Warm Config reveal does not grant foreground permission before signaling");
}

void testNativeOwnerPreservesQtInputAndPopupAssociation()
{
    if (QGuiApplication::platformName().compare(
        QStringLiteral("windows"), Qt::CaseInsensitive) != 0)
        return;

    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"VP test owner",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 200, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    require(owner != nullptr, "Cannot create native VP owner fixture");
    testAdvertisedEditor = nullptr;
    testOwnerOriginalProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        owner, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(testOwnerProcedure)));
    require(testOwnerOriginalProcedure != nullptr,
        "Cannot install native VP owner association fixture");
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, reinterpret_cast<quintptr>(owner), true);
    window.reveal();
    QCoreApplication::processEvents();

    HWND editor = reinterpret_cast<HWND>(window.effectiveWinId());
    const HWND actualOwner = GetWindow(editor, GW_OWNER);
    if (actualOwner != nullptr)
        throw std::runtime_error(QStringLiteral(
            "Config editor unexpectedly acquired a native owner: editor=%1 expected=%2 actual=%3")
            .arg(reinterpret_cast<quintptr>(editor), 0, 16)
            .arg(static_cast<quintptr>(0), 0, 16)
            .arg(reinterpret_cast<quintptr>(actualOwner), 0, 16)
            .toStdString());
    require((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
        "Explicit reveal did not apply scoped topmost above fullscreen");
    require(appearsAbove(editor, owner),
        "Visible VP-associated Config was not above its video owner fixture");
	HWND competingTopmost = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"VP fullscreen cover fixture",
		WS_POPUP, 0, 0, 320, 200, nullptr, nullptr,
		GetModuleHandleW(nullptr), nullptr);
	require(competingTopmost != nullptr,
		"Cannot create competing topmost renderer fixture");
	ShowWindow(competingTopmost, SW_SHOWNOACTIVATE);
	SetWindowPos(competingTopmost, HWND_TOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	require(appearsAbove(competingTopmost, editor),
		"Competing topmost fixture did not cover Config before the retry");
	window.reveal();
	QCoreApplication::processEvents();
	require(appearsAbove(editor, competingTopmost),
		"Repeated explicit reveal did not force Config back to the top");
	DestroyWindow(competingTopmost);
    require(testAdvertisedEditor == editor,
        "Config editor did not advertise its current native top-level HWND");

    QLineEdit* shortcut = requireControl<QLineEdit>(window,
        QStringLiteral("config.shortcuts.fullscreen_toggle"));
    shortcut->setFocus();
    shortcut->selectAll();
    QKeyEvent keyPress(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier,
        QStringLiteral("z"));
    QApplication::sendEvent(shortcut, &keyPress);
    require(shortcut->text() == QStringLiteral("z"),
        "Native association interfered with normal Qt edit keyboard input");

    QComboBox* renderer = requireControl<QComboBox>(window,
        QStringLiteral("config.general.video_conversion"));
    for (QWidget* ancestor = renderer->parentWidget(); ancestor;
        ancestor = ancestor->parentWidget())
    {
        if (auto* scrollArea = qobject_cast<QScrollArea*>(ancestor))
        {
            scrollArea->ensureWidgetVisible(renderer);
            break;
        }
    }
    QCoreApplication::processEvents();
    require(renderer->isVisibleTo(&window),
        "Real Video conversion dropdown is not visible in Config");
    SetForegroundWindow(editor);
    window.raise();
    window.activateWindow();
    renderer->setFocus();
    QCoreApplication::processEvents();
    SetForegroundWindow(editor);
    QCoreApplication::processEvents();
    renderer->showPopup();
    const ULONGLONG openDeadline = GetTickCount64() + 500;
    while (!renderer->view()->window()->isVisible() &&
        GetTickCount64() < openDeadline)
    {
        QCoreApplication::processEvents();
        Sleep(10);
    }
    QWidget* popup = renderer->view()->window();
    require(popup != nullptr,
        "Native association did not preserve the Qt combo popup window");
    require(popup->isVisible(), "Renderer popup did not open for lease test");
    const ULONGLONG popupDeadline = GetTickCount64() + 1100;
    while (GetTickCount64() < popupDeadline)
    {
        QCoreApplication::processEvents();
        Sleep(20);
    }
    const HWND popupWindow = reinterpret_cast<HWND>(popup->effectiveWinId());
    require(popup->isVisible() && popupWindow && IsWindow(popupWindow),
        "Natural owner chain did not preserve the native combo popup");
    require(renderer->count() > 1,
        "Video conversion dropdown has no alternate mouse choice");
    const int previousRenderer = renderer->currentIndex();
    const int selectedRenderer = previousRenderer == 0 ? 1 : 0;
    const QModelIndex selectedIndex = renderer->model()->index(selectedRenderer,
        renderer->modelColumn(), renderer->rootModelIndex());
    const QPoint clickPoint = renderer->view()->visualRect(selectedIndex).center();
    QWidget* viewport = renderer->view()->viewport();
    const HWND viewportWindow = reinterpret_cast<HWND>(viewport->winId());
    SendMessageW(viewportWindow, WM_MOUSEMOVE, 0,
        MAKELPARAM(clickPoint.x(), clickPoint.y()));
    SendMessageW(viewportWindow, WM_LBUTTONDOWN, MK_LBUTTON,
        MAKELPARAM(clickPoint.x(), clickPoint.y()));
    SendMessageW(viewportWindow, WM_LBUTTONUP, 0,
        MAKELPARAM(clickPoint.x(), clickPoint.y()));
    QCoreApplication::processEvents();
    require(renderer->currentIndex() == selectedRenderer && !popup->isVisible(),
        "Mouse click could not select and close the real Video conversion dropdown");
    require((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
        "Main editor lost scoped topmost during popup selection");

    QComboBox* hdrColorspace = requireControl<QComboBox>(window,
        QStringLiteral("config.general.hdr_colorspace"));
    require(hdrColorspace->isVisibleTo(&window),
        "Real HDR color space dropdown was not accessible after mouse selection");
    hdrColorspace->setFocus();
    QKeyEvent openPopup(QEvent::KeyPress, Qt::Key_Down, Qt::AltModifier);
    QApplication::sendEvent(hdrColorspace, &openPopup);
    QCoreApplication::processEvents();
    QWidget* hdrPopup = hdrColorspace->view()->window();
    require(hdrPopup && hdrPopup->isVisible(),
        "Keyboard could not open the real HDR color space dropdown");
    QKeyEvent navigatePopup(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(hdrColorspace->view(), &navigatePopup);
    QKeyEvent escapePopup(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(hdrColorspace->view(), &escapePopup);
    QCoreApplication::processEvents();
    require(!hdrPopup->isVisible(),
        "Keyboard Escape did not close the real HDR color space dropdown normally");
    renderer->showPopup();
    QCoreApplication::processEvents();
    require(popup->isVisible(), "Renderer popup did not reopen for activation cleanup");
    const UINT activationMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.Activate.v1");
    DWORD_PTR acknowledged = 0;
    require(SendMessageTimeoutW(editor, activationMessage,
        GetCurrentProcessId(), reinterpret_cast<LPARAM>(owner),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledged) &&
        acknowledged == 1,
        "Activation was not acknowledged before queuing reveal");
    QCoreApplication::processEvents();
    require(window.isVisible() && !popup->isVisible(),
        "Queued activation did not close the stale combo popup and reveal");

    window.hide();
    require((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0 &&
        GetWindow(editor, GW_OWNER) == nullptr,
        "Hiding Config did not clear its native owner and normal z-order state");
    acknowledged = 0;
    require(SendMessageTimeoutW(editor, activationMessage,
        GetCurrentProcessId(), reinterpret_cast<LPARAM>(owner),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledged) &&
        acknowledged == 1 && !window.isVisible(),
        "Repeated activation did not acknowledge before deferred reveal");
    QCoreApplication::processEvents();
    require(window.isVisible(), "Deferred repeated activation did not reveal the editor");
    window.close();
    require(!window.isVisible() &&
        (GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0,
        "Closing Config did not remove its temporary topmost state");
    window.reveal();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    editor = reinterpret_cast<HWND>(window.effectiveWinId());
    require(window.isVisible() &&
        (GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
        "Explicit reveal did not restore scoped topmost state");

    // Force qwindows to destroy and recreate the native top level. VP must be
    // told the effective replacement HWND before the next activation.
    window.hide();
    window.setWindowFlag(Qt::Tool, true);
    window.show();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    editor = reinterpret_cast<HWND>(window.effectiveWinId());
    require(editor && IsWindow(editor) && testAdvertisedEditor == editor,
        "WinId recreation did not publish the replacement editor HWND");
    acknowledged = 0;
    require(SendMessageTimeoutW(testAdvertisedEditor, activationMessage,
        GetCurrentProcessId(), reinterpret_cast<LPARAM>(owner),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledged) &&
        acknowledged == 1,
        "Activation through the advertised replacement HWND was not acknowledged");
    QCoreApplication::processEvents();
    require(window.isVisible(),
        "Activation through the advertised replacement HWND did not reveal");
    shortcut->setFocus();
    shortcut->selectAll();
    QKeyEvent secondKeyPress(QEvent::KeyPress, Qt::Key_Y, Qt::NoModifier,
        QStringLiteral("y"));
    QApplication::sendEvent(shortcut, &secondKeyPress);
    require(shortcut->text() == QStringLiteral("y"),
        "Repeated activation interfered with normal Qt keyboard focus");

    HWND replacementHost = CreateWindowExW(WS_EX_TOOLWINDOW,
        L"STATIC", L"replacement VP fullscreen host", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(replacementHost != nullptr, "Cannot create replacement video host fixture");
    ShowWindow(replacementHost, SW_SHOWNOACTIVATE);
    acknowledged = 0;
    require(SendMessageTimeoutW(editor, activationMessage,
        GetCurrentProcessId(), reinterpret_cast<LPARAM>(replacementHost),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledged) && acknowledged == 1,
        "Config did not acknowledge replacement VP host association");
    QCoreApplication::processEvents();
    editor = reinterpret_cast<HWND>(window.effectiveWinId());
    const UINT targetMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.PresentationTarget.v1");
    DWORD_PTR targetAcknowledged = 0;
    require(SendMessageTimeoutW(editor, targetMessage, GetCurrentProcessId(),
        reinterpret_cast<LPARAM>(replacementHost),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &targetAcknowledged) &&
        targetAcknowledged == 1,
        "Replacement presentation target was not accepted");
	const UINT targetMessageV2 = RegisterWindowMessageW(
		L"VideoProcessor.ConfigEditor.PresentationTarget.v2");
	const UINT acknowledgementEndpointMessage = RegisterWindowMessageW(
		L"VideoProcessor.ConfigEditor.PresentationTargetAckEndpoint.v1");
	constexpr uint32_t targetSequence = 0x141;
	testPresentationTargetAcknowledgementSequence = 0;
	testPresentationTargetAcknowledgementEditor = nullptr;
	require(PostMessageW(editor, acknowledgementEndpointMessage,
		static_cast<WPARAM>(GetCurrentProcessId()),
		reinterpret_cast<LPARAM>(owner)),
		"Could not queue asynchronous presentation-target acknowledgement endpoint");
	require(PostMessageW(editor, targetMessageV2,
		(static_cast<WPARAM>(targetSequence) << 32) |
			static_cast<WPARAM>(GetCurrentProcessId()),
		reinterpret_cast<LPARAM>(replacementHost)),
		"Could not queue asynchronous presentation-target v2 update");
	QEventLoop acknowledgementLoop;
	QTimer acknowledgementPoll;
	QObject::connect(&acknowledgementPoll, &QTimer::timeout,
		&acknowledgementLoop, [&acknowledgementLoop, targetSequence]
		{
			if (testPresentationTargetAcknowledgementSequence == targetSequence)
				acknowledgementLoop.quit();
		});
	QTimer::singleShot(1000, &acknowledgementLoop, &QEventLoop::quit);
	acknowledgementPoll.start(10);
	acknowledgementLoop.exec();
	require(testPresentationTargetAcknowledgementSequence == targetSequence &&
		testPresentationTargetAcknowledgementEditor == editor,
		"Presentation-target v2 update did not asynchronously acknowledge the current Config HWND");
    QCoreApplication::processEvents();
    require(GetWindow(editor, GW_OWNER) == nullptr &&
        (GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
        "Config did not remain independently topmost after host replacement");
    SetWindowPos(replacementHost, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	window.reveal();
    QCoreApplication::processEvents();
    require(appearsAbove(editor, replacementHost),
		"Explicit reveal did not restore Config above the video host");
	const UINT reassertMessage = RegisterWindowMessageW(
		L"VideoProcessor.ConfigEditor.Reassert.v1");
	const BOOL foregroundChanged = SetForegroundWindow(owner);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
	require((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
        "External foreground unexpectedly removed visible Config topmost state");
	if (foregroundChanged)
		require(GetForegroundWindow() == owner,
			"Windows accepted the foreground change but Config immediately stole it back");

    // Renderer/host lifecycle traffic may reassert z-order while Config is
    // visible, but it must not steal foreground from the user's current app.
    PostMessageW(editor, reassertMessage, GetCurrentProcessId(),
        reinterpret_cast<LPARAM>(replacementHost));
    Sleep(100);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
	require((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
        "Lifecycle reassert stole foreground or removed visible Config topmost state");
	if (foregroundChanged)
		require(GetForegroundWindow() == owner,
			"Lifecycle reassert stole an established external foreground");

    window.reveal();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    editor = reinterpret_cast<HWND>(window.effectiveWinId());
    require((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
        "Explicit reveal did not reapply scoped topmost after a gated lifecycle reassert");
    window.hide();
    QCoreApplication::processEvents();
    require((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0,
        "Hiding Config after explicit reveal did not remove scoped topmost state");
    DestroyWindow(replacementHost);
    Sleep(250);
    QCoreApplication::processEvents();
    require((GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0,
        "Destroyed VP owner left Config in the topmost band");
    window.hide();
    SetWindowLongPtrW(owner, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(testOwnerOriginalProcedure));
    testOwnerOriginalProcedure = nullptr;
    testAdvertisedEditor = nullptr;
	testPresentationTargetAcknowledgementSequence = 0;
	testPresentationTargetAcknowledgementEditor = nullptr;
    DestroyWindow(owner);
}

void testRealConfigurationDropdownRemainsClickable()
{
    const bool nativeWindows = QGuiApplication::platformName().compare(
        QStringLiteral("windows"), Qt::CaseInsensitive) == 0;
    HWND owner = nativeWindows ? CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
        L"VP dropdown owner", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    require(!nativeWindows || owner != nullptr,
        "Cannot create dropdown owner fixture");
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, reinterpret_cast<quintptr>(owner), true);
    window.show();
    QCoreApplication::processEvents();

    QComboBox* conversion = requireControl<QComboBox>(window,
        QStringLiteral("config.general.video_conversion"));
    for (QWidget* ancestor = conversion->parentWidget(); ancestor;
        ancestor = ancestor->parentWidget())
    {
        if (auto* scrollArea = qobject_cast<QScrollArea*>(ancestor))
        {
            scrollArea->ensureWidgetVisible(conversion);
            break;
        }
    }
    QCoreApplication::processEvents();
    require(conversion->isVisibleTo(&window) && conversion->count() > 1,
        "Real Video conversion dropdown is unavailable");
    conversion->showPopup();
    QCoreApplication::processEvents();
    QWidget* conversionPopup = conversion->view()->window();
    require(conversionPopup && conversionPopup->isVisible(),
        "Real Video conversion dropdown did not open");
	HWND firstPresentationTarget = nullptr;
	HWND secondPresentationTarget = nullptr;
	if (nativeWindows)
	{
		firstPresentationTarget = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
			L"VP transient renderer one", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
			nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		secondPresentationTarget = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
			L"VP transient renderer two", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
			nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		require(firstPresentationTarget && secondPresentationTarget,
			"Cannot create transient renderer fixtures");
		const UINT targetMessage = RegisterWindowMessageW(
			L"VideoProcessor.ConfigEditor.PresentationTarget.v1");
		for (const HWND target : { firstPresentationTarget,
			secondPresentationTarget })
		{
			DWORD_PTR acknowledged = 0;
			require(SendMessageTimeoutW(
				reinterpret_cast<HWND>(window.effectiveWinId()), targetMessage,
				GetCurrentProcessId(), reinterpret_cast<LPARAM>(target),
				SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledged) &&
				acknowledged == 1,
				"Transient renderer target change was not acknowledged");
			QCoreApplication::processEvents();
			require(conversionPopup->isVisible(),
				"Renderer target change closed an active Config dropdown");
			require(GetWindow(reinterpret_cast<HWND>(window.effectiveWinId()),
				GW_OWNER) == nullptr,
				"Renderer target change assigned Config a native owner");
		}
	}
    const ULONGLONG openUntil = GetTickCount64() + 1100;
    while (GetTickCount64() < openUntil)
    {
        QCoreApplication::processEvents();
        Sleep(20);
    }
    require(conversionPopup->isVisible(),
        "Real Video conversion dropdown could not remain open");

    const int original = conversion->currentIndex();
    const int alternate = original == 0 ? 1 : 0;
    const QModelIndex alternateIndex = conversion->model()->index(alternate,
        conversion->modelColumn(), conversion->rootModelIndex());
    const QPoint point = conversion->view()->visualRect(alternateIndex).center();
    QWidget* conversionViewport = conversion->view()->viewport();
    if (nativeWindows)
    {
        const HWND viewport = reinterpret_cast<HWND>(conversionViewport->winId());
        SendMessageW(viewport, WM_MOUSEMOVE, 0, MAKELPARAM(point.x(), point.y()));
        SendMessageW(viewport, WM_LBUTTONDOWN, MK_LBUTTON,
            MAKELPARAM(point.x(), point.y()));
        SendMessageW(viewport, WM_LBUTTONUP, 0, MAKELPARAM(point.x(), point.y()));
    }
    else
    {
        const QPoint globalPoint = conversionViewport->mapToGlobal(point);
        QMouseEvent move(QEvent::MouseMove, QPointF(point), QPointF(globalPoint),
            Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(conversionViewport, &move);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(point),
            QPointF(globalPoint), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(conversionViewport, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point),
            QPointF(globalPoint), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(conversionViewport, &release);
    }
    QCoreApplication::processEvents();
    require(conversion->currentIndex() == alternate &&
        !conversionPopup->isVisible(),
        "A native mouse click could not select the real Video conversion dropdown");

    QComboBox* hdr = requireControl<QComboBox>(window,
        QStringLiteral("config.general.hdr_colorspace"));
    QKeyEvent openHdr(QEvent::KeyPress, Qt::Key_Down, Qt::AltModifier);
    QApplication::sendEvent(hdr, &openHdr);
    QCoreApplication::processEvents();
    QWidget* hdrPopup = hdr->view()->window();
    require(hdrPopup && hdrPopup->isVisible(),
        "Real HDR color space dropdown was inaccessible after mouse selection");
    QKeyEvent navigateHdr(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(hdr->view(), &navigateHdr);
    QKeyEvent closeHdr(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(hdr->view(), &closeHdr);
    QCoreApplication::processEvents();
    require(!hdrPopup->isVisible(),
        "Real HDR color space dropdown did not close normally");

    QComboBox* container = requireControl<QComboBox>(window,
        QStringLiteral("config.general.container_colorspace"));
    container->showPopup();
    QCoreApplication::processEvents();
    require(container->view()->window()->isVisible(),
        "A subsequent real dropdown was no longer accessible");
    container->hidePopup();
    window.close();
	if (firstPresentationTarget)
		DestroyWindow(firstPresentationTarget);
	if (secondPresentationTarget)
		DestroyWindow(secondPresentationTarget);
    if (owner)
        DestroyWindow(owner);
}

void testColdHiddenAssociationDeliversPendingReveal()
{
    if (QGuiApplication::platformName().compare(
        QStringLiteral("windows"), Qt::CaseInsensitive) != 0)
        return;

    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
        L"VP cold-start owner", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(owner != nullptr, "Cannot create cold-start VP owner fixture");
    testAdvertisedEditor = nullptr;
    testAssociationActivationAcknowledged = false;
    testActivateOnAssociation = true;
    testOwnerOriginalProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        owner, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(testOwnerProcedure)));
    require(testOwnerOriginalProcedure != nullptr,
        "Cannot install cold-start association fixture");
    ShowWindow(owner, SW_SHOWNOACTIVATE);

    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, reinterpret_cast<quintptr>(owner), true);
    for (int attempt = 0; attempt < 4; ++attempt)
        QCoreApplication::processEvents();
    HWND editor = reinterpret_cast<HWND>(window.effectiveWinId());
    require(editor && IsWindow(editor) && testAdvertisedEditor == editor,
        "Cold hidden editor did not advertise its first usable native HWND");
    require(testAssociationActivationAcknowledged && window.isVisible(),
        "Pending activation immediately after association was not acknowledged and revealed");

    // Model an older tray-close already queued when the newer runtime reveal
    // arrives. The newer reveal must remain the final visible state.
    window.hide();
    QCoreApplication::postEvent(&window, new QCloseEvent());
    const UINT activationMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.Activate.v1");
    DWORD_PTR acknowledged = 0;
    require(SendMessageTimeoutW(editor, activationMessage,
        GetCurrentProcessId(), reinterpret_cast<LPARAM>(owner),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &acknowledged) &&
        acknowledged == 1 && !window.isVisible(),
        "New reveal did not acknowledge before superseding queued close");
    QCoreApplication::processEvents();
    require(window.isVisible(),
        "An older queued close hid the editor after a successful reveal");

    window.hide();
    SetWindowLongPtrW(owner, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(testOwnerOriginalProcedure));
    testOwnerOriginalProcedure = nullptr;
    testAdvertisedEditor = nullptr;
    testActivateOnAssociation = false;
    DestroyWindow(owner);
}

void testStableRevealEndpointSurvivesHiddenQtWindow()
{
	if (QGuiApplication::platformName().compare(
		QStringLiteral("windows"), Qt::CaseInsensitive) != 0)
		return;

	HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
		L"VP stable reveal owner", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
		nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	require(owner != nullptr, "Cannot create stable reveal owner fixture");
	QTemporaryDir directory;
	ConfigEditorWindow window(copyFixture(directory),
		reinterpret_cast<quintptr>(owner), true);
	window.show();
	QCoreApplication::processEvents();
	window.hide();
	QCoreApplication::processEvents();
	require(!window.isVisible(), "Config did not enter its hidden tray state");

	const std::wstring eventName =
		ConfigurationLiveApply::ConfigurationEditorRevealEventName(
			GetCurrentProcessId());
	HANDLE revealEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE,
		eventName.c_str());
	require(revealEvent != nullptr,
		"Stable Config reveal endpoint was not created");
	require(SetEvent(revealEvent) != FALSE,
		"Stable Config reveal endpoint could not be signaled");
	CloseHandle(revealEvent);
	const ULONGLONG deadline = GetTickCount64() + 1000;
	while (!window.isVisible() && GetTickCount64() < deadline)
	{
		QCoreApplication::processEvents();
		Sleep(5);
	}
	require(window.isVisible(),
		"Stable process endpoint did not reveal hidden Config");
	const HWND revealedEditor = reinterpret_cast<HWND>(window.effectiveWinId());
	const BOOL foregroundAccepted = SetForegroundWindow(revealedEditor);
	const ULONGLONG stabilityDeadline = GetTickCount64() + 3500;
	while (GetTickCount64() < stabilityDeadline)
	{
		QCoreApplication::processEvents();
		Sleep(10);
	}
	require(reinterpret_cast<HWND>(window.effectiveWinId()) == revealedEditor &&
		IsWindow(revealedEditor),
		"Config recreated its native HWND after a settled reveal");
	require((GetWindowLongPtrW(revealedEditor, GWL_EXSTYLE) &
		WS_EX_TOPMOST) != 0,
		"Config lost topmost placement after a settled reveal");
	if (foregroundAccepted)
		require(GetForegroundWindow() == revealedEditor,
			"Config lost foreground without user input after a settled reveal");
	window.hide();
	DestroyWindow(owner);
}

void testCrossThreadActivationHasNoReverseCallbackDeadlock()
{
    if (QGuiApplication::platformName().compare(
        QStringLiteral("windows"), Qt::CaseInsensitive) != 0)
        return;

    ReverseCallbackFixture fixture;
    std::thread ownerThread([&fixture]
    {
        const wchar_t* className = L"VP.ConfigEditor.ReverseCallbackTest";
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = reverseCallbackOwnerProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        RegisterClassW(&windowClass);
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, className,
            L"VP reverse-callback owner", WS_OVERLAPPEDWINDOW,
            0, 0, 320, 200, nullptr, nullptr, windowClass.hInstance, &fixture);
        fixture.owner.store(owner);
        if (!owner) return;
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });

    const ULONGLONG ownerDeadline = GetTickCount64() + 2000;
    while (!fixture.owner.load() && GetTickCount64() < ownerDeadline)
        Sleep(1);
    const HWND owner = fixture.owner.load();
    bool advertisedCurrent = false;
    bool becameVisible = false;
    if (owner)
    {
        QTemporaryDir directory;
        const QString path = copyFixture(directory);
        ConfigEditorWindow window(path, reinterpret_cast<quintptr>(owner), true);
        const ULONGLONG activationDeadline = GetTickCount64() + 2000;
        while ((!fixture.activationReturned.load() || !window.isVisible()) &&
            GetTickCount64() < activationDeadline)
        {
            QCoreApplication::processEvents();
            Sleep(1);
        }
        advertisedCurrent = fixture.advertisedEditor.load() ==
            reinterpret_cast<HWND>(window.effectiveWinId());
        becameVisible = window.isVisible();
        window.hide();
        PostMessageW(owner, WM_CLOSE, 0, 0);
    }
    if (ownerThread.joinable()) ownerThread.join();

    require(owner != nullptr, "Cannot create reverse-callback owner thread fixture");
    require(fixture.activationReturned.load(),
        "Cross-thread Activate timed out in a reverse Association callback cycle");
    require(fixture.activationAcknowledged.load(),
        "Cross-thread Activate did not return ack=1");
    require(fixture.activationElapsedMs.load() < 250,
        "Cross-thread Activate did not acknowledge promptly");
    require(advertisedCurrent,
        "Cross-thread association did not advertise the current editor HWND");
    require(becameVisible,
        "Queued reveal did not make the cross-thread activated editor visible");
}

void testExternalForegroundLeavesConfigTopmost()
{
    if (QGuiApplication::platformName().compare(
        QStringLiteral("windows"), Qt::CaseInsensitive) != 0)
        return;

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW receiverClass{};
    receiverClass.lpfnWndProc = externalFixtureReceiverProcedure;
    receiverClass.hInstance = instance;
    receiverClass.lpszClassName = L"VP.ConfigTests.FixtureReceiver.v1";
    RegisterClassW(&receiverClass);
    ExternalZOrderFixture fixture;
    HWND receiver = CreateWindowExW(WS_EX_TOOLWINDOW,
        receiverClass.lpszClassName, L"fixture receiver", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance, &fixture);
    require(receiver != nullptr, "Cannot create external fixture receiver");

    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(executable) +
        L"\" --zorder-helper " +
        std::to_wstring(reinterpret_cast<quintptr>(receiver));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool launched = CreateProcessW(nullptr, command.data(), nullptr,
        nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    bool popupKeptForeground = false;
    bool externalForegroundKeepsConfigTopmost = false;
    bool hostUnchanged = false;
    bool editorAboveReactivatedHost = false;
    bool spoofedTargetRejected = false;
    bool targetPlacementValid = false;
    bool independentOwnerPreserved = false;
    bool targetMainIsScoped = false;
    bool manualMovePreserved = false;
    bool repeatRevealClamped = false;
    if (launched)
    {
        const ULONGLONG readyDeadline = GetTickCount64() + 3000;
        while ((!fixture.owner || !fixture.host) && GetTickCount64() < readyDeadline)
        {
            QCoreApplication::processEvents();
            Sleep(1);
        }
        if (fixture.owner && fixture.host)
        {
            RECT hostBefore{};
            GetWindowRect(fixture.host, &hostBefore);
            const LONG_PTR styleBefore = GetWindowLongPtrW(fixture.host, GWL_STYLE);
            const LONG_PTR exStyleBefore = GetWindowLongPtrW(fixture.host, GWL_EXSTYLE);
            QTemporaryDir directory;
            ConfigEditorWindow window(copyFixture(directory),
                reinterpret_cast<quintptr>(fixture.owner), true);
            HWND editor = reinterpret_cast<HWND>(window.effectiveWinId());
            static const UINT targetMessage = RegisterWindowMessageW(
                L"VideoProcessor.ConfigEditor.PresentationTarget.v1");
            DWORD_PTR targetAck = 1;
            spoofedTargetRejected = SendMessageTimeoutW(editor, targetMessage,
                GetCurrentProcessId(), reinterpret_cast<LPARAM>(fixture.host),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &targetAck) && targetAck == 0;
            targetAck = 0;
            require(SendMessageTimeoutW(editor, targetMessage,
                process.dwProcessId, reinterpret_cast<LPARAM>(fixture.host),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &targetAck) && targetAck == 1,
                "Valid presentation target handoff was not acknowledged");
            RECT hiddenFrame{};
            GetWindowRect(editor, &hiddenFrame);
            const int preservedWidth = hiddenFrame.right - hiddenFrame.left;
            const int preservedHeight = hiddenFrame.bottom - hiddenFrame.top;
            window.reveal();
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
            editor = reinterpret_cast<HWND>(window.effectiveWinId());
            RECT placedFrame{};
            GetWindowRect(editor, &placedFrame);
            MONITORINFO targetMonitor{};
            targetMonitor.cbSize = sizeof(targetMonitor);
            GetMonitorInfoW(MonitorFromWindow(fixture.host,
                MONITOR_DEFAULTTONEAREST), &targetMonitor);
            const RECT work = targetMonitor.rcWork;
            targetPlacementValid = placedFrame.left >= work.left &&
                placedFrame.top >= work.top && placedFrame.right <= work.right &&
                placedFrame.bottom <= work.bottom &&
                placedFrame.right - placedFrame.left == preservedWidth &&
                placedFrame.bottom - placedFrame.top == preservedHeight;
            independentOwnerPreserved = GetWindow(editor, GW_OWNER) == nullptr;
            targetMainIsScoped =
                (GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

            SetForegroundWindow(editor);

            QWidget popup(&window, Qt::Popup);
            popup.resize(160, 80);
            popup.show();
            const HWND popupWindow = reinterpret_cast<HWND>(popup.winId());
            SetForegroundWindow(popupWindow);
            Sleep(250);
            QCoreApplication::processEvents();
            popupKeptForeground = popup.isVisible();

            AllowSetForegroundWindow(process.dwProcessId);
            static const UINT requestMessage = RegisterWindowMessageW(
                L"VideoProcessor.ConfigTests.ZOrderFixture.RequestForeground.v1");
            PostMessageW(fixture.host, requestMessage, 0, 0);
            // A visible Config window is now intentionally system-topmost.
            // The host may or may not receive foreground under Windows focus
            // policy, but it must not demote Config from the topmost band.
            Sleep(250);
            Sleep(250);
            QCoreApplication::processEvents();
            externalForegroundKeepsConfigTopmost =
                (GetWindowLongPtrW(editor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            RECT hostAfter{};
            GetWindowRect(fixture.host, &hostAfter);
            hostUnchanged = EqualRect(&hostBefore, &hostAfter) &&
                GetWindowLongPtrW(fixture.host, GWL_STYLE) == styleBefore &&
                GetWindowLongPtrW(fixture.host, GWL_EXSTYLE) == exStyleBefore &&
                (exStyleBefore & WS_EX_TOPMOST) != 0 &&
                (styleBefore & WS_POPUP) != 0;
            popup.hide();
            SetForegroundWindow(editor);
            AllowSetForegroundWindow(process.dwProcessId);
            PostMessageW(fixture.host, requestMessage, 0, 0);
            const ULONGLONG foregroundDeadline = GetTickCount64() + 1500;
            while (GetTickCount64() < foregroundDeadline)
            {
                QCoreApplication::processEvents();
                Sleep(5);
            }
            // A topmost style bit alone is insufficient: a second topmost
            // fullscreen window can still cover Config after VP is activated.
            editorAboveReactivatedHost = appearsAbove(editor, fixture.host);

            SetWindowPos(editor, nullptr, work.right - 100, work.bottom - 100,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT manuallyMoved{};
            GetWindowRect(editor, &manuallyMoved);
            Sleep(250);
            QCoreApplication::processEvents();
            RECT afterLease{};
            GetWindowRect(editor, &afterLease);
            manualMovePreserved = EqualRect(&manuallyMoved, &afterLease) != FALSE;
            window.hide();
            window.reveal();
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
            GetWindowRect(editor, &placedFrame);
            repeatRevealClamped = placedFrame.left >= work.left &&
                placedFrame.top >= work.top && placedFrame.right <= work.right &&
                placedFrame.bottom <= work.bottom;
            window.hide();
        }
        PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(process.hProcess, 3000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    DestroyWindow(receiver);

    require(launched, "Cannot launch external VP z-order fixture process");
    require(fixture.owner && fixture.host,
        "External VP z-order fixture did not publish its windows");
    require(spoofedTargetRejected,
        "Presentation target handoff accepted a spoofed process id");
    require(targetPlacementValid,
        "Reveal did not preserve size and fully clamp Config to target monitor work area");
    require(independentOwnerPreserved,
        "Presentation target handoff assigned Config a native owner");
    require(targetMainIsScoped,
        "Presentation-owned Config was not scoped above topmost fullscreen");
    require(manualMovePreserved,
        "Visible Config was pulled back after a manual move");
    require(repeatRevealClamped,
        "Re-hide/reveal did not return Config to the presentation target monitor");
    require(popupKeptForeground,
        "Natural Config ownership closed a same-process popup unexpectedly");
    require(externalForegroundKeepsConfigTopmost,
        "Visible Config lost topmost placement after external foreground activation");
    require(hostUnchanged,
        "Config foreground recovery changed fullscreen host rect or styles");
    require(editorAboveReactivatedHost,
        "Reactivating VP placed its topmost fullscreen host above Config");
}

void testSyntheticPresentationTargetClamp()
{
    const QRect workArea(-1920, 40, 1920, 1040);
    const QRect offscreenFrame(1700, 900, 1040, 700);
    const QRect placed = ConfigEditorPlacement::ClampFrameToWorkArea(
        offscreenFrame, workArea);
    require(placed.size() == offscreenFrame.size(),
        "Presentation placement changed the Config window size");
    require(workArea.contains(placed),
        "Presentation placement did not fully clamp to a synthetic monitor work area");
    if (GetSystemMetrics(SM_CMONITORS) < 2)
        std::cout << "INFO physical two-monitor placement skipped: only one monitor; "
            "synthetic negative-origin clamp plus live target HWND coverage ran" << std::endl;
}

void testVisiblePresentationTargetUpdateMovesEditor()
{
    if (QGuiApplication::platformName().compare(
        QStringLiteral("windows"), Qt::CaseInsensitive) != 0 ||
        GetSystemMetrics(SM_CMONITORS) < 2)
        return;

    std::vector<RECT> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitorRects,
        reinterpret_cast<LPARAM>(&monitors));
    require(monitors.size() >= 2,
        "Two-monitor placement fixture could not enumerate two monitors");

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
        L"VP placement owner", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
        nullptr, nullptr, instance, nullptr);
    HWND firstTarget = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
        L"VP first presentation target", WS_POPUP,
        monitors[0].left, monitors[0].top, 320, 200,
        nullptr, nullptr, instance, nullptr);
    HWND secondTarget = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC",
        L"VP second presentation target", WS_POPUP,
        monitors[1].left, monitors[1].top, 320, 200,
        nullptr, nullptr, instance, nullptr);
    require(owner && firstTarget && secondTarget,
        "Cannot create presentation-target placement fixtures");
    ShowWindow(firstTarget, SW_SHOWNOACTIVATE);
    ShowWindow(secondTarget, SW_SHOWNOACTIVATE);

    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory),
        reinterpret_cast<quintptr>(owner), true);
    const HWND editor = reinterpret_cast<HWND>(window.effectiveWinId());
    const UINT targetMessage = RegisterWindowMessageW(
        L"VideoProcessor.ConfigEditor.PresentationTarget.v1");
    auto updateTarget = [editor, targetMessage](HWND target)
    {
        DWORD_PTR acknowledged = 0;
        require(SendMessageTimeoutW(editor, targetMessage, GetCurrentProcessId(),
            reinterpret_cast<LPARAM>(target), SMTO_ABORTIFHUNG | SMTO_BLOCK,
            1000, &acknowledged) && acknowledged == 1,
            "Presentation target update was not accepted");
        QCoreApplication::processEvents();
    };

    updateTarget(firstTarget);
    window.reveal();
    QCoreApplication::processEvents();
    require(MonitorFromWindow(editor, MONITOR_DEFAULTTONEAREST) ==
        MonitorFromWindow(firstTarget, MONITOR_DEFAULTTONEAREST),
        "Initial Config reveal did not use the current VP monitor");

    updateTarget(secondTarget);
    require(MonitorFromWindow(editor, MONITOR_DEFAULTTONEAREST) ==
        MonitorFromWindow(secondTarget, MONITOR_DEFAULTTONEAREST),
        "Visible Config did not move when VP's presentation monitor changed");

    window.hide();
    DestroyWindow(secondTarget);
    DestroyWindow(firstTarget);
    DestroyWindow(owner);
}

void testNormalWindowArchitectureHasNoLeasePolling()
{
    const QByteArray source = readBytes(repositoryPath(QStringLiteral(
        "src/VideoProcessor-Config/ConfigEditorWindow.cpp")));
    require(!source.contains("ownerMonitor_") &&
        !source.contains("maintainForegroundLease") &&
        !source.contains("maintainPopupZOrder") &&
        !source.contains("setInterval(200)"),
        "Config editor still contains polling, popup, or foreground lease logic");
}

void testShaderProfileIntentIsRetainedBeforeRendererConstruction()
{
    const QByteArray source = readBytes(repositoryPath(QStringLiteral(
        "src/VideoProcessor-GUI/VideoProcessorDlg.cpp")));
    const qsizetype functionStart = source.indexOf(
        "void CVideoProcessorDlg::ApplyUnifiedProfileSnapshot(");
    const qsizetype functionEnd = source.indexOf(
        "void CVideoProcessorDlg::QueueUnifiedQueueProfileReset(",
        functionStart);
    require(functionStart >= 0 && functionEnd > functionStart,
        "Cannot locate unified profile snapshot application");
    const QByteArray applyPath = source.mid(functionStart,
        functionEnd - functionStart);
    const qsizetype retain = applyPath.indexOf(
        "m_requestedShaderProfiles = std::move(shaderProfiles);");
    const qsizetype transitionGuard = applyPath.indexOf(
        "const bool selectedRendererDiffers");
    const qsizetype missingRendererGuard = applyPath.indexOf(
        "if (!m_videoRenderer)");
    const qsizetype apply = applyPath.indexOf(
        "m_videoRenderer->SelectShaderProfiles(");
    require(retain >= 0 && transitionGuard > retain &&
        missingRendererGuard > retain && apply > missingRendererGuard,
        "Shader profile intent is not retained before renderer-transition "
        "and missing-renderer exits");
}

void testSharedProfileListControllerContract()
{
    QWidget parent;
    QListWidget list;
    list.setDragDropMode(QAbstractItemView::InternalMove);
    QPushButton add;
    QPushButton remove;
    QPushButton up;
    QPushButton down;
    QStringList sections = { QStringLiteral("profile.root"),
        QStringLiteral("profile.second") };
    int dirtyCount = 0;
    int detailLoads = 0;

    ProfileListPolicy policy;
    policy.sections = [&sections] { return sections; };
    policy.displayName = [](const QString& section)
        { return section.section(u'.', -1); };
    policy.addProfile = [&sections]
    {
        const QString section = QStringLiteral("profile.third");
        sections.push_back(section);
        return section;
    };
    policy.removeProfile = [&sections](const QString& section)
    {
        return sections.removeOne(section);
    };
    policy.normalizeForOrdering = [](const QString& section)
        { return section; };
    policy.moveAfter = [&sections](const QString& wanted,
        const QString& previous)
    {
        if (wanted == previous) return false;
        const int previousIndex = sections.indexOf(previous);
        const int wantedIndex = sections.indexOf(wanted);
        if (previousIndex < 0 || wantedIndex < 0) return false;
        sections.removeAt(wantedIndex);
        sections.insert(sections.indexOf(previous) + 1, wanted);
        return true;
    };
    policy.markDirty = [&dirtyCount] { ++dirtyCount; };

    ProfileListController controller(&parent, &parent, &list, &add, &remove,
        &up, &down, std::move(policy));
    controller.setCurrentChanged([&detailLoads](QListWidgetItem*)
        { ++detailLoads; });
    controller.refresh(QStringLiteral("profile.root"));
    require(list.count() == 2 && list.currentRow() == 0,
        "Shared profile component did not load the requested edit selection");
    require(list.item(0)->text() == QStringLiteral("root  (Default)"),
        "Shared profile component did not derive Default from first position");
    require(remove.isEnabled() && !up.isEnabled() && down.isEnabled(),
        "Shared profile component exposed incorrect first-row actions");

    controller.updateCurrentLabel(QStringLiteral("Live name"));
    require(list.item(0)->text() == QStringLiteral("Live name  (Default)"),
        "Shared profile component did not update its current name immediately");
    // audit 2026-10-16: a move loads only the moved row, never the neighbour
    // takeItem briefly makes current.
    QStringList loaded;
    QObject::connect(&list, &QListWidget::currentItemChanged, &parent,
        [&loaded](QListWidgetItem* current)
        { if (current) loaded.push_back(current->data(Qt::UserRole).toString()); });
    down.click();
    require(sections == QStringList{ QStringLiteral("profile.second"),
        QStringLiteral("profile.root") } && list.currentRow() == 1,
        "Shared profile component did not persist button reordering");
    require(!loaded.contains(QStringLiteral("profile.second")),
        "Moving a profile loaded its neighbour into the editor");

    require(list.model()->moveRow(QModelIndex(), 1, QModelIndex(), 0),
        "Shared profile component rejected model drag reordering");
    require(sections.front() == QStringLiteral("profile.root") &&
        list.currentRow() == 0,
        "Shared profile component did not persist drag reordering");

    add.click();
    require(sections.back() == QStringLiteral("profile.third") &&
        list.currentItem()->data(Qt::UserRole).toString() ==
            QStringLiteral("profile.third"),
        "Shared profile component did not add and select a profile");
    answerMessageBox(QMessageBox::Yes);
    remove.click();
    require(!sections.contains(QStringLiteral("profile.third")) &&
        list.count() == 2,
        "Shared profile component did not remove its selected profile");
    require(dirtyCount >= 4 && detailLoads >= 5,
        "Shared profile component did not publish mutations and selection loads");
}

void testLimitedTransportGammaNoticeAndCorrection()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create transport fixture");
    file.write("[vprenderer.Default]\nsdr_target_nits: 100\nsdr_black_nits: 0\n"
        "[vprenderer.color.First]\noutput_gamma: 2.2\noutput_range: limited\noutput_transport_gamma: 2.4\ncalibration_lut_enabled: false\n"
        "[vprenderer.color.Second]\noutput_transport_gamma: 2.4\n");
    file.close();
    const auto original = readBytes(path);
    {
        ConfigEditorWindow window(path, 0, true);
        auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.color.profiles");
        auto* display = requireControl<QComboBox>(window, "config.vprenderer.color.output_gamma");
        auto* range = requireControl<QComboBox>(window, "config.vprenderer.color.output_range");
        auto* transport = requireControl<QComboBox>(window, "config.vprenderer.color.output_transport_gamma");
        auto* flag = requireControl<QCheckBox>(window, "config.vprenderer.color.diagnostic_allow_limited_g22");
        auto* notice = requireControl<QLabel>(window, "config.vprenderer.color.transport_gamma_notice");
        auto* match = requireControl<QPushButton>(window, "config.vprenderer.color.match_transport_gamma");
        require(notice->text().contains("gamma mismatch") && !match->isHidden(), "Missing explicit gamma mismatch and correction");
        profiles->setCurrentRow(1);
        require(notice->text().contains("display gamma 2.2") && match->isEnabled(), "Inherited display/range did not drive notice");
        require(readBytes(path) == original, "Inspecting profiles changed saved settings");
        match->click();
        require(transport->currentData() == "2.2" && flag->isChecked(), "Correction did not update transport and beta flag");
        require(notice->text().contains("not an exact pure-2.2") && match->isHidden(), "G22 incorrectly reported as exact agreement");
        selectData(display, "2.4");
        match->click();
        require(transport->currentData() == "2.4" && !flag->isChecked(), "G24 correction did not clear beta flag");
        selectData(display, "2.6");
        require(notice->text().contains("no matching declaration") && match->isHidden(), "Unsupported gamma offered an invented correction");
        requireControl<QRadioButton>(window, "config.vprenderer.color.calibration_method.lut")->click();
        require(notice->text().contains("cannot be inferred") && match->isHidden(), "LUT input gamma treated as its output gamma");
        requireControl<QRadioButton>(window, "config.vprenderer.color.calibration_method.vp")->click();
        selectData(range, "full");
        require(notice->isHidden() && match->isHidden(), "Full output has a Limited warning");
        selectData(display, "2.2");
        selectData(range, "limited");
        match->click();
        save(window);
    }
    const auto saved = readBytes(path).replace("\r\n", "\n");
    require(saved.contains("[vprenderer.color.first]\noutput_gamma: 2.2\noutput_range: limited\noutput_transport_gamma: 2.4") ||
        saved.contains("[vprenderer.color.First]\noutput_gamma: 2.2\noutput_range: limited\noutput_transport_gamma: 2.4"), "Correction changed a different profile");
    ConfigEditorWindow reloaded(path, 0, true);
    requireControl<QListWidget>(reloaded, "config.vprenderer.color.profiles")->setCurrentRow(1);
    require(requireControl<QComboBox>(reloaded, "config.vprenderer.color.output_gamma")->currentData() == "2.2" &&
        requireControl<QComboBox>(reloaded, "config.vprenderer.color.output_transport_gamma")->currentData() == "2.2" &&
        requireControl<QCheckBox>(reloaded, "config.vprenderer.color.diagnostic_allow_limited_g22")->isChecked(), "Correction failed to persist");
}

void testCalibratedUiDefaultsAndDerivedTransport()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("VideoProcessor.cfg"));
    {
        ConfigEditorWindow window(path, 0, true);
        window.selectPage(13);
        require(requireControl<QStackedWidget>(window, QStringLiteral("settingsPages"))->currentIndex() == 16,
            "Old Output page link did not open the combined page");
        auto combo = [&](const char* name) { return requireControl<QComboBox>(window, QString::fromLatin1(name)); };
        require(combo("config.vprenderer.color.output_gamma")->currentData() == QStringLiteral("2.2"), "Fresh display gamma is not 2.2");
        require(combo("config.vprenderer.color.sdr_input_transfer")->currentData() == QStringLiteral("2.4"), "Fresh input gamma is not 2.4");
        require(combo("config.vprenderer.display_bit_depth")->currentData() == QStringLiteral("10"), "Fresh dither depth is not 10");
        require(requireControl<QLineEdit>(window, QStringLiteral("config.vprenderer.sdr_target_nits"))->text() == QStringLiteral("100"), "Fresh target nits is not 100");
        require(requireControl<QLineEdit>(window, QStringLiteral("config.vprenderer.sdr_black_nits"))->text() == QStringLiteral("0"), "Fresh target black is not 0");
        auto* range = combo("config.vprenderer.color.output_range");
        auto* gamma = combo("config.vprenderer.color.output_transport_gamma");
        auto* flag = requireControl<QCheckBox>(window, QStringLiteral("config.vprenderer.color.diagnostic_allow_limited_g22"));
        require(range->currentData() == QStringLiteral("full") && !gamma->isEnabled(), "Full does not disable Limited transport");
        require(!qobject_cast<QStandardItemModel*>(combo("config.vprenderer.color.output_gamma")->model())->item(0)->isEnabled(), "Fresh root omission can recreate removed Auto");
        require(range->findData(QStringLiteral("AUTO")) < 0 && combo("config.vprenderer.color.output_gamma")->findData(QStringLiteral("AUTO")) < 0, "Fresh calibrated selector exposes Auto");
        selectData(range, QStringLiteral("limited"));
        require(gamma->isEnabled() && flag->isChecked() && !flag->isEnabled(), "Limited2.2 flag not derived");
        selectData(gamma, QStringLiteral("2.4"));
        require(!flag->isChecked(), "Limited2.4 did not clear flag");
        selectData(gamma, QStringLiteral("2.2"));
        selectData(range, QStringLiteral("full"));
        require(!flag->isChecked() && gamma->currentData() == QStringLiteral("2.2"), "Full did not preserve dormant transfer and clear flag");
        save(window);
    }
    const QByteArray saved = readBytes(path);
    require(saved.contains("sdr_target_nits: 100") && saved.contains("sdr_black_nits: 0") && saved.contains("output_gamma: 2.2") && saved.contains("diagnostic_allow_limited_g22: false"), "Fresh defaults did not persist");
    ConfigEditorWindow reloaded(path, 0, true);
    auto* range = requireControl<QComboBox>(reloaded, QStringLiteral("config.vprenderer.color.output_range"));
    selectData(range, QStringLiteral("limited"));
    save(reloaded);
    require(readBytes(path).contains("diagnostic_allow_limited_g22: true"), "Reloaded Limited selection did not persist its flag");
}

void testCalibratedLegacyAndInheritedOutputPreserved()
{
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("VideoProcessor.cfg"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create legacy fixture");
    file.write("[vprenderer.Default]\nsdr_target_nits: 175\nsdr_black_nits: auto\n[vprenderer.color.Default]\noutput_gamma: auto\nsdr_input_transfer: 2.4\noutput_range: limited\noutput_transport_gamma: 2.2\ndiagnostic_allow_limited_g22: false\n[vprenderer.color.Full]\noutput_range: full\n[vprenderer.color.Unrelated]\noutput_range: limited\noutput_transport_gamma: 2.2\ndiagnostic_allow_limited_g22: false\n");
    file.close();
    ConfigEditorWindow window(path, 0, true);
    auto* flag = requireControl<QCheckBox>(window, QStringLiteral("config.vprenderer.color.diagnostic_allow_limited_g22"));
    require(!flag->isChecked(), "Opening legacy mismatch silently enabled experiment");
    // Opening writes nothing unrequested (audit 2026-10-17): save only when a migration is pending.
    if (requireControl<QPushButton>(window, "applyConfiguration")->isEnabled()) save(window);
    require(readBytes(path).contains("output_gamma: auto") && readBytes(path).contains("sdr_target_nits: 175"), "Legacy calibration changed");
    auto* gamma = requireControl<QComboBox>(window, QStringLiteral("config.vprenderer.color.output_transport_gamma"));
    selectData(gamma, QStringLiteral("2.4"));
    selectData(gamma, QStringLiteral("2.2"));
    require(flag->isChecked(), "Edited base did not enable Limited2.2");
    auto* profiles = requireControl<QListWidget>(window, QStringLiteral("config.vprenderer.color.profiles"));
    profiles->setCurrentRow(1);
    require(!flag->isChecked() && !gamma->isEnabled(), "Full child inherited incompatible flag");
    profiles->setCurrentRow(2);
    require(!flag->isChecked(), "Editing base repaired an unrelated legacy gate");
    requireControl<QPushButton>(window, QStringLiteral("config.vprenderer.color.add_profile"))->click();
    profiles->setCurrentRow(2);
    require(!flag->isChecked(), "Adding profile repaired an unrelated legacy gate");
    require(QMetaObject::invokeMethod(gamma, "activated", Qt::DirectConnection, Q_ARG(int, gamma->currentIndex())), "Cannot activate existing transport choice");
    require(flag->isChecked(), "Reselecting legacy2.2 did not synchronize gate");
    save(window);
    ConfigEditorWindow reloaded(path, 0, true);
    auto* reloadedProfiles = requireControl<QListWidget>(reloaded, QStringLiteral("config.vprenderer.color.profiles"));
    reloadedProfiles->setCurrentRow(1);
    require(!requireControl<QCheckBox>(reloaded, QStringLiteral("config.vprenderer.color.diagnostic_allow_limited_g22"))->isChecked(), "Child flag changed on reload");
}

void testUnifiedColorOutputMigrationAndTransferRoundTrip()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "Cannot create migration fixture");
    const QByteArray original = R"cfg([vprenderer.Default]
quality: high
[vprenderer.color.Rec709]
shortcut: F5
output_gamma: 2.2
sdr_input_transfer: bt1886
[vprenderer.color.BT2020]
shortcut: F6
sdr_target_primaries: bt2020
[vprenderer.output.First]
# retain output comment
output_range: limited
output_transport_gamma: 2.2
diagnostic_allow_limited_g22: true
diagnostic_allow_full_g22: true
shortcut: F9
[vprenderer.output.Unused]
# preserve inactive custom setting
unknown_future_key: retained
output_range: full
)cfg";
    file.write(original); file.close();
    ConfigEditorWindow window(path, 0, true);
    require(window.findChild<QListWidget*>("config.vprenderer.output.profiles") == nullptr,
        "An independent Output profile list remains");
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.color.profiles");
    require(profiles->count() == 2, "Migration changed Color profile count");
    auto* range = requireControl<QComboBox>(window, "config.vprenderer.color.output_range");
    require(range->currentData().toString() == "limited", "First Output was not copied to first Color");
    profiles->setCurrentRow(1);
    require(range->currentData().toString() == "limited", "First Output was not copied to second Color");
    require(requireControl<QLineEdit>(window,"config.vprenderer.color.shortcut")->text() == "F6",
        "Output shortcut replaced Color shortcut");
    selectData(requireControl<QComboBox>(window,"config.vprenderer.color.hdr_tone_map_target_gamma"), "2.6");
    requireControl<QCheckBox>(window,"config.vprenderer.color.sdr_adjust_gamma")->setCheckState(Qt::Unchecked);
    save(window);
    const QByteArray saved = readBytes(path);
    require(saved.contains("[legacy_output.first]") && saved.contains("[legacy_output.unused]") &&
        saved.contains("# retain output comment") && saved.contains("unknown_future_key: retained") &&
        saved.contains("diagnostic_allow_full_g22: true"), "Migration discarded old settings/comments");
    const auto backups = QDir(directory.path()).entryList({"*.before-config-migration.*.bak"},QDir::Files);
    require(backups.size() == 1 && readBytes(directory.filePath(backups.front())) == original,
        "Migration did not preserve an exact original-file backup");
    ConfigEditorWindow reloaded(path, 0, true);
    requireControl<QListWidget>(reloaded,"config.vprenderer.color.profiles")->setCurrentRow(1);
    require(requireControl<QComboBox>(reloaded,"config.vprenderer.color.hdr_tone_map_target_gamma")->currentData().toString() == "2.6",
        "LUT domain changed on reload");
    require(requireControl<QCheckBox>(reloaded,"config.vprenderer.color.sdr_adjust_gamma")->checkState() == Qt::Unchecked,
        "Explicit pass-through changed on reload");
    require(!requireControl<QPushButton>(reloaded, "applyConfiguration")->isEnabled(),
        "Reload unexpectedly scheduled another migration");
    ConfigEditorCore::ConfigDocument unchanged;
    ConfigEditorCore::SaveResult result;
    std::wstring error;
    require(unchanged.Load(path.toStdWString(), error), "Cannot reload migrated document");
    require(ConfigEditorCore::SaveSafely(unchanged, result, error), "Cannot save migrated document");
    require(result.backupPath.empty(), "Second save unnecessarily repeated migration backup");
    require(readBytes(path) == saved, "Migration/save is not idempotent");
}

void testTargetDropdownRefreshesInline()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    QList<ConfigEditorWindow::Target> discovered = {
        { QStringLiteral("remote-a"), QStringLiteral("STUDY (192.168.1.20)"),
            QStringLiteral("192.168.1.20"), 41686 },
        { QStringLiteral("remote-b"), QStringLiteral("DEN (192.168.1.21)"),
            QStringLiteral("192.168.1.21"), 41686 }
    };
    int scans = 0;
    window.setTargetRefresh([&] { ++scans; });
    window.setDiscoveredTargets(discovered);
    auto* choice = requireControl<QComboBox>(window, "config.targetChoice");
    auto* refresh = requireControl<QToolButton>(window, "config.refreshTargets");
    auto* caption = requireControl<QLabel>(window, "configTargetCaption");
    require(choice->currentText() == QStringLiteral("This computer") &&
        choice->count() == 4 &&
        choice->itemText(1).startsWith(QStringLiteral("STUDY")) &&
        choice->itemText(2).startsWith(QStringLiteral("DEN")),
        "Discovered targets were not offered in the header dropdown");
    require(caption->text() == QStringLiteral("Configure VP:") &&
        choice->property("targetSelector").toBool() &&
        refresh->property("targetRefresh").toBool() &&
        !refresh->icon().isNull(),
        "The compact target label or styled refresh control is missing");
    choice->ensurePolished();
    caption->ensurePolished();
    require(choice->height() == 24 && refresh->height() == 24 &&
        refresh->iconSize() == QSize(14, 14) &&
        choice->font().pixelSize() == caption->font().pixelSize(),
        "Target selector and refresh control are larger than the caption");
    require(refresh->toolTip().contains(QStringLiteral("Refresh")) &&
        scans == 0, "Target refresh ran before the user requested it");
    window.show();
    QApplication::processEvents();
    choice->showPopup();
    QApplication::processEvents();
    auto* popup = choice->view();
    const int finalRowBottom = popup->visualRect(
        popup->model()->index(choice->count() - 1, 0)).bottom();
    require(popup->viewport()->height() > finalRowBottom &&
        !popup->horizontalScrollBar()->isVisible(),
        "The target popup clipped its final choice");
    const QString captures = qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
    if (!captures.isEmpty())
        popup->window()->grab().save(QDir(captures).filePath(
            QStringLiteral("target-popup-after-fix.png")));
    choice->hidePopup();
    choice->clearFocus();
    QApplication::processEvents();
    const QImage selector = choice->grab().toImage();
    require(selector.pixel(0, selector.height() / 2) ==
            selector.pixel(selector.width() - 1, selector.height() / 2),
        "The target selector's right border is missing");
    if (!captures.isEmpty())
        selector.save(QDir(captures).filePath(
            QStringLiteral("target-selector-corners.png")));
    const QColor leftCorner(selector.pixel(1, 1));
    const QColor rightCorner(selector.pixel(selector.width() - 2, 1));
    require(rightCorner.red() > 25 &&
            std::abs(leftCorner.red() - rightCorner.red()) < 20 &&
            std::abs(leftCorner.green() - rightCorner.green()) < 20,
        "The arrow fill clipped the target selector's rounded corner");
    window.hide();
    discovered.removeLast();
    refresh->click();
    window.setDiscoveredTargets(discovered);
    require(scans == 1 && choice->count() == 3 &&
        choice->currentText() == QStringLiteral("This computer") &&
        QApplication::activeModalWidget() == nullptr,
        "Refreshing targets did not update the dropdown in place");
    window.setDiscoveredTargets({
        { QStringLiteral("local-installation"), QStringLiteral("This computer (127.0.0.1)"),
            QStringLiteral("127.0.0.1"), 1 }, discovered.front() });
    require(choice->count() == 3,
        "The current VP appeared twice after LAN discovery");
    window.setDiscoveredTargets({
        { QStringLiteral("local-installation"), QStringLiteral("This computer (192.168.1.25)"),
			QStringLiteral("192.168.1.25"), 1, {}, true }, discovered.front() });
    require(choice->count() == 3 &&
        choice->currentText() == QStringLiteral("This computer"),
        "A restarted VP was treated as a second target after its address changed");
}

void exerciseRemoteTargetSwitch(bool automatic)
{
    using namespace ConfigurationRpcProtocol;
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    const std::string remoteBytes = readBytes(path).toStdString();
    WSADATA data{};
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0,
        "Cannot initialize the RPC stall fixture");
    struct WinsockCleanup { ~WinsockCleanup() { WSACleanup(); } } cleanup;
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "Cannot open the RPC stall listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(listener, reinterpret_cast<sockaddr*>(&address),
        sizeof(address)) == 0 && listen(listener, 1) == 0,
        "Cannot listen for the stalled RPC request");
    int addressSize = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
        &addressSize) == 0, "Cannot read the RPC stall port");
    // Flaky-test fix 2026-10-18: the first reply is held until the test has seen the
    // loading screen (release), instead of a fixed 450 ms that a loaded machine
    // could outlast; the server ends when the test is done (finished), not after
    // a fixed accept timeout between the window's requests.
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::thread stalledServer([listener, path = path.toStdString(), remoteBytes, release, finished]
    {
        const auto receiveExact = [](SOCKET socket, uint8_t* bytes, size_t count)
        {
            while (count != 0)
            {
                const int received = recv(socket,
                    reinterpret_cast<char*>(bytes), static_cast<int>(count), 0);
                if (received <= 0) return false;
                bytes += received;
                count -= received;
            }
            return true;
        };
        for (int request = 0; request < 3; ++request)
        {
            // The window's next request can follow the reply by seconds on a loaded
            // machine: wait until the test is done (bounded at 60 s).
            int ready = 0;
            for (int waitedMs = 0; ready == 0 && !finished->load() && waitedMs < 60000; waitedMs += 100)
            {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(listener, &readable);
                timeval timeout{ 0, 100000 };
                ready = select(0, &readable, nullptr, nullptr, &timeout);
            }
            if (ready <= 0) break;
            const SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            std::array<uint8_t, HeaderBytes> header{};
            uint8_t compatibility[2]{};
            if (!receiveExact(client, header.data(), header.size()) ||
                !receiveExact(client, compatibility, sizeof(compatibility)))
            {
                closesocket(client);
                break;
            }
            const uint16_t operation = Read16(header.data() + 6);
            Frame response;
            response.operation = operation | ResponseFlag;
            if (operation == static_cast<uint16_t>(Operation::GetCapabilities))
            {
                Write16(response.payload, ConfigurationCompatibilityVersion);
                WriteString(response.payload, "1.3.test");
                for (int list = 0; list < 6; ++list)
                    Write32(response.payload, 0);
            }
            else if (operation == static_cast<uint16_t>(Operation::GetConfig))
            {
                WriteString(response.payload, path);
                WriteString(response.payload, remoteBytes);
            }
            std::vector<uint8_t> wire;
            Encode(response, wire);
            if (request == 0)
                for (int waitedMs = 0; !release->load() && !finished->load() && waitedMs < 60000; waitedMs += 5)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            size_t sent = 0;
            while (sent < wire.size())
            {
                const int count = send(client,
                    reinterpret_cast<const char*>(wire.data() + sent),
                    static_cast<int>(wire.size() - sent), 0);
                if (count <= 0) break;
                sent += count;
            }
            closesocket(client);
        }
        closesocket(listener);
    });
    struct ServerDone
    {
        std::shared_ptr<std::atomic<bool>> release, finished;
        ~ServerDone() { release->store(true); finished->store(true); }
    };
    struct ThreadJoin
    {
        std::thread& thread;
        ServerDone done;
        ~ThreadJoin() { done.release->store(true); done.finished->store(true); if (thread.joinable()) thread.join(); }
    } join{ stalledServer, { release, finished } };

    ConfigEditorWindow window(path, 0, true, {}, {},
        {}, 41686, {}, true, !automatic);
    // Ends the stall before the window (whose destructor waits for its request)
    // goes, also when a check below fails.
    ServerDone releaseFirst{ release, finished };
    window.show();
    QApplication::processEvents();
    QElapsedTimer dispatch;
    dispatch.start();
    window.setDiscoveredTargets({
        { QStringLiteral("stalled"), QStringLiteral("SLOW VP"),
            QStringLiteral("localhost"), ntohs(address.sin_port) }
    });
    auto* choice = requireControl<QComboBox>(window, "config.targetChoice");
    require(choice->count() == (automatic ? 3 : 4) &&
        choice->itemText(automatic ? 1 : 2) ==
        QStringLiteral("SLOW VP"), "Stalled VP was not offered in the selector");
    if (!automatic)
    {
        dispatch.restart();
        QMetaObject::invokeMethod(choice, "activated", Qt::DirectConnection,
            Q_ARG(int, 2));
    }
    // The first reply is held until released below, so a connection made on the
    // UI thread would block here until the server's 60 s bound: 5 s still tells
    // the two apart on a loaded machine (was 250 ms against a 450 ms stall).
    require(dispatch.elapsed() < 5000,
        "Connecting to a remote VP blocked the UI thread");
    auto* overlay = requireControl<QWidget>(window,
        "config.targetSwitchOverlay");
    auto* title = requireControl<QLabel>(window,
        "config.targetSwitchTitle");
    require(overlay->isVisible() && title->text().contains("SLOW VP"),
        "Remote switching did not show its loading screen");
    bool heartbeat = false;
    QEventLoop eventLoop;
    QTimer::singleShot(30, &window, [&] { heartbeat = true; });
    QTimer::singleShot(100, &eventLoop, &QEventLoop::quit);
    eventLoop.exec();
    require(heartbeat && overlay->isVisible(),
        "The loading screen did not stay responsive during the stalled request");
    const QString captures = qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
    if (!automatic && !captures.isEmpty())
        window.grab().save(QDir(captures).filePath(
            QStringLiteral("target-switch-loading.png")));
    window.resize(1140, 760);
    QApplication::processEvents();
    require(overlay->geometry() == window.centralWidget()->rect(),
        "The loading screen did not follow a window resize");
    release->store(true);   // the stalled reply goes out now
    QElapsedTimer completion;
    completion.start();
    while (window.findChild<QWidget*>("config.targetSwitchOverlay") &&
        completion.elapsed() < 30000)
    {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(!window.findChild<QWidget*>("config.targetSwitchOverlay") &&
        requireControl<QComboBox>(window, "config.targetChoice")->currentText() ==
            QStringLiteral("SLOW VP") &&
        requireControl<QPushButton>(window, "okConfiguration")->isEnabled() &&
        !window.awaitingTarget(),
        "Successful remote connection did not open the target configuration");
}

void testRemoteTargetSwitchKeepsWindowResponsive()
{
    exerciseRemoteTargetSwitch(false);
}

void testRemoteOnlyAutomaticallyOpensSoleVp()
{
    exerciseRemoteTargetSwitch(true);
}

void testNoTargetStartupIsQuiet()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true, {}, {},
        {}, 41686, {}, true);
    auto* choice = requireControl<QComboBox>(window, "config.targetChoice");
    auto* title = requireControl<QLabel>(window, "config.targetSearchTitle");
    auto* progress = requireControl<QProgressBar>(window, "configTargetSearchProgress");
    require(choice->currentText() == QStringLiteral("Searching…") &&
        title->text() == QStringLiteral("Looking for VideoProcessor…") &&
        progress->minimum() == 0 && progress->maximum() == 0 &&
        !progress->isHidden() &&
        !requireControl<QPushButton>(window, "applyConfiguration")->isEnabled() &&
        !requireControl<QPushButton>(window, "okConfiguration")->isEnabled() &&
        QApplication::activeModalWidget() == nullptr,
        "No-target startup opened a prompt or allowed configuration writes");
    // RC3: with no target only Updates stays clickable (page 21 after the
    // merge; page 20 is Dynamic Drive).
    auto* updatesEntry = requireControl<QPushButton>(window, "config.openUpdates");
    require(updatesEntry->isEnabled(), "Updates is not clickable without a target");
    for (auto* button : window.findChildren<QPushButton*>())
        if (button != updatesEntry && button->property("pageIndex").isValid())
            require(!button->isEnabled(), QStringLiteral("Page %1 is clickable without a target")
                .arg(button->property("pageIndex").toInt()).toStdString().c_str());
    const QString captures = qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
    if (!captures.isEmpty())
    {
        window.show();
        QApplication::processEvents();
        window.grab().save(QDir(captures).filePath(
            QStringLiteral("target-search-loading.png")));
        window.hide();
    }
    window.setDiscoveredTargets({});
    require(choice->currentText() == QStringLiteral("Searching…"),
        "An empty first scan hid the loading state");
    window.finishInitialTargetSearch();
    require(choice->currentText() == QStringLiteral("No target"),
        "The completed search did not show a quiet no-target state");
    require(title->text() == QStringLiteral("No VideoProcessor found") &&
        progress->isHidden(),
        "The search progress stayed visible after the initial search");
    window.setDiscoveredTargets({
        { QStringLiteral("remote-a"), QStringLiteral("STUDY"),
            QStringLiteral("192.168.1.20"), 41686 },
        { QStringLiteral("remote-b"), QStringLiteral("DEN"),
            QStringLiteral("192.168.1.21"), 41686 }
    });
    require(title->text() == QStringLiteral("Choose a VideoProcessor") &&
        choice->currentText() == QStringLiteral("Choose target"),
        "An available VP was still reported as not found after discovery");
}

void testOfflineFileStillOffersTargets()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true);
    auto* choice = requireControl<QComboBox>(window, "config.targetChoice");
    auto* refresh = requireControl<QToolButton>(window, "config.refreshTargets");
    require(choice->currentText() == QStringLiteral("This computer") &&
        requireControl<QPushButton>(window, "okConfiguration")->isEnabled(),
        "The local file was not editable without a running VP");
    int scans = 0;
    window.setTargetRefresh([&] { ++scans; });
    window.setDiscoveredTargets({
        { QStringLiteral("local-installation"), QStringLiteral("This computer (127.0.0.1)"),
			QStringLiteral("127.0.0.1"), 41686, {}, true }
    });
    require(choice->count() == 2 && choice->currentText() == QStringLiteral("This computer"),
        "Discovery duplicated the local file target");
    window.show();
    QApplication::processEvents();
    choice->showPopup();
    QApplication::processEvents();
    auto* popup = choice->view();
    require(popup->viewport()->height() > popup->visualRect(
            popup->model()->index(choice->count() - 1, 0)).bottom(),
        "The two-choice target popup clipped Enter address");
    choice->hidePopup();
    window.hide();
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    if (apply->isEnabled()) apply->click(); // Commit fixture migrations.
    auto* fullscreen = requireControl<QCheckBox>(window,
        QStringLiteral("config.general.fullscreen"));
    const bool changed = !fullscreen->isChecked();
    fullscreen->setChecked(changed);
    require(apply->isEnabled(), "Local edit did not enable Apply");
    apply->click();
    require(!apply->isEnabled(), "Local Apply did not save while VP was absent");
    ConfigEditorCore::ConfigDocument saved;
    std::wstring error;
    require(saved.Load(path.toStdWString(), error) &&
        saved.Get("general", "fullscreen") == (changed ? "true" : "false"),
        "Offline local Apply did not persist the edited configuration");
    window.setDiscoveredTargets({
        { QStringLiteral("remote-a"), QStringLiteral("STUDY (192.168.1.20)"),
            QStringLiteral("192.168.1.20"), 41686 }
    });
    require(choice->count() == 3 &&
        choice->itemText(1).startsWith(QStringLiteral("STUDY")),
        "Local file mode did not offer a discovered remote VP");
    refresh->click();
    require(scans == 1 && QApplication::activeModalWidget() == nullptr,
		"Local file mode did not refresh targets inline");
}

void testUnavailableRpcCanReturnToLocalFile()
{
    QTemporaryDir directory;
    const QString path = copyFixture(directory);
    ConfigEditorWindow window(path, 0, true, {}, {},
        QStringLiteral("127.0.0.1"), 1, QStringLiteral("This computer"));
    auto* choice = requireControl<QComboBox>(window, "config.targetChoice");
    require(choice->itemText(1) == QStringLiteral("This computer"),
        "Unavailable RPC target did not offer the local file");
    QElapsedTimer dispatch;
    dispatch.start();
    QMetaObject::invokeMethod(choice, "activated", Qt::DirectConnection,
        Q_ARG(int, 1));
    require(dispatch.elapsed() < 250 &&
        window.findChild<QWidget*>("config.targetSwitchOverlay"),
        "Local target loading blocked the UI or skipped its loading screen");
    QElapsedTimer completion;
    completion.start();
    while (window.findChild<QWidget*>("config.targetSwitchOverlay") &&
        completion.elapsed() < 5000)
    {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(requireControl<QComboBox>(window, "config.targetChoice")->currentText() ==
            QStringLiteral("This computer") &&
        requireControl<QPushButton>(window, "okConfiguration")->isEnabled(),
        "Selecting This computer did not recover the editable file after RPC failure");
}

void testRememberedTargetDoesNotFallBackToLocal()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true, {}, {},
        {}, 41686, {}, true, true,
        QStringLiteral("old-computer-no-longer-on-lan"),
        QStringLiteral("STUDY"));
    auto* choice = requireControl<QComboBox>(window, "config.targetChoice");
    require(choice->currentText() == QStringLiteral("STUDY (searching…)") &&
        choice->itemText(1) == QStringLiteral("This computer"),
        "The remembered remote did not remain selected while searching");
    window.setDiscoveredTargets({
        { QStringLiteral("local-installation"), QStringLiteral("This computer"),
            QStringLiteral("127.0.0.1"), 1, {}, true }
    });
    require(choice->currentText() == QStringLiteral("STUDY (searching…)") &&
        !requireControl<QPushButton>(window, "okConfiguration")->isEnabled(),
        "The remembered remote silently fell back to the local file");
    window.finishInitialTargetSearch();
    require(requireControl<QLabel>(window, "config.targetSearchTitle")->text()
            .contains(QStringLiteral("STUDY is unavailable")) &&
        choice->currentText() == QStringLiteral("STUDY (unavailable)"),
        "An unavailable remembered target was not explained");
    const QString captures = qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
    if (!captures.isEmpty())
    {
        window.show();
        QApplication::processEvents();
        window.grab().save(QDir(captures).filePath(
            QStringLiteral("remembered-target-unavailable.png")));
        window.hide();
    }
    QMetaObject::invokeMethod(choice, "activated", Qt::DirectConnection,
        Q_ARG(int, 1));
    QElapsedTimer completion;
    completion.start();
    while (window.findChild<QWidget*>("config.targetSwitchOverlay") &&
        completion.elapsed() < 5000)
    {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(requireControl<QComboBox>(window, "config.targetChoice")->currentText() ==
            QStringLiteral("This computer") &&
        requireControl<QPushButton>(window, "okConfiguration")->isEnabled(),
        "The user could not explicitly choose local editing");
}

void testRemoteOnlyClientHasNoLocalFileTarget()
{
    QTemporaryDir directory;
    ConfigEditorWindow window(copyFixture(directory), 0, true, {}, {},
        {}, 41686, {}, true, false);
    auto* choice = requireControl<QComboBox>(window, "config.targetChoice");
    require(choice->count() == 2 &&
        choice->itemText(1) == QStringLiteral("Enter address…") &&
        !requireControl<QPushButton>(window, "okConfiguration")->isEnabled(),
        "A remote-only Config copy offered a local file target");
    window.setDiscoveredTargets({
        { QStringLiteral("lan-target"), QStringLiteral("LIVING ROOM"),
            QStringLiteral("localhost"), 1, {}, false }
    });
    require(window.findChild<QWidget*>("config.targetSwitchOverlay") &&
        choice->itemText(1) == QStringLiteral("LIVING ROOM") &&
        !requireControl<QPushButton>(window, "okConfiguration")->isEnabled(),
        "A remote-only first launch did not connect to its sole VP");
    QElapsedTimer completion;
    completion.start();
    while (window.findChild<QWidget*>("config.targetSwitchOverlay") &&
        completion.elapsed() < 10000)
    {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    window.finishInitialTargetSearch();
    require(!window.findChild<QWidget*>("config.targetSwitchOverlay") &&
        window.awaitingTarget() &&
        requireControl<QLabel>(window, "config.targetSearchTitle")->text() ==
            QStringLiteral("Choose a VideoProcessor") &&
        requireControl<QComboBox>(window, "config.targetChoice")->itemText(1) ==
            QStringLiteral("LIVING ROOM"),
        "A failed connection incorrectly said no VP was discovered");
    window.setDiscoveredTargets({
        { QStringLiteral("lan-target"), QStringLiteral("LIVING ROOM"),
            QStringLiteral("localhost"), 1, {}, false },
        { QStringLiteral("other-target"), QStringLiteral("STUDY"),
            QStringLiteral("other-host"), 41686, {}, false }
    });
    require(!window.findChild<QWidget*>("config.targetSwitchOverlay") &&
        requireControl<QComboBox>(window, "config.targetChoice")->count() == 4,
        "Multiple remote targets were selected without user choice");
}


void testSubtitleProfileTabRoundTrip()
{
    QTemporaryDir dir;
    const QString path=dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly),"Cannot write subtitle profile fixture");
    file.write("[general]\nrenderer: VideoProcessor Renderer (Alpha)\n"
        "[vprenderer.zoom.First]\nsubtitle_fit: true\nautomatic_crop: true\n"
        "[vprenderer.subtitles.Classic]\ntype: classic\noffset_pixels: 0\ncycle_shortcut: Ctrl+Shift+T\n"
        "[vprenderer.subtitles.Gray]\ntype: generated_gray\noffset_pixels: 15\ncycle_shortcut: Ctrl+Shift+T\n");file.close();
    ConfigEditorWindow window(path,0,true);
    window.show(); QApplication::processEvents();
    auto* pages=requireControl<QStackedWidget>(window,"settingsPages");pages->setCurrentIndex(19);
    auto* list=requireControl<QListWidget>(window,"config.vprenderer.subtitles.profiles");
    require(list->count()==2,"Subtitle profiles missing");list->setCurrentRow(1);
    auto* type=requireControl<QComboBox>(window,"config.vprenderer.subtitles.type");
    auto* offset=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.offset_pixels");
    require(type->count()==4 && type->currentData().toString()=="generated_gray","Four subtitle styles not resolved");
    require(offset->text()=="15","Profile offset missing");
    require(type->itemText(1)=="Classic" && type->itemText(2)=="Move / Reconstruction (Beta)" && type->itemText(3)=="Move / Solid Background (Beta)","Subtitle labels do not match their stored types");
    auto* textColor=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.subtitle_text_color");
    require(textColor->text()=="FFFFFF","Text color must default to original white");
    textColor->setText("B0B0B0");
    requireControl<QPushButton>(window,"config.vprenderer.subtitles.subtitle_text_color.picker.toggle");
    auto* color=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.subtitle_generated_gray_color");
    auto* blur=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.subtitle_generated_gray_blur_px");
    require(blur->isVisible(),"Reconstruction controls hidden");
    auto* choose=requireControl<QPushButton>(window,"config.vprenderer.subtitles.subtitle_generated_gray_color.picker.toggle");
    choose->click();QApplication::processEvents();
    auto* picker=dynamic_cast<InlineColorEditor*>(requireControl<QWidget>(window,"config.vprenderer.subtitles.subtitle_generated_gray_color.picker.editor"));
    require(picker!=nullptr,"Compact editor missing");
    require(!picker->isWindow() && picker->isVisible(),"Color picker must be embedded and non-modal");
    require(picker->findChildren<QLineEdit*>().isEmpty() && picker->findChildren<QPushButton*>().size()==48,
        "Compact picker must contain only basic swatches and color surfaces, without custom/numeric controls");
    requireControl<QWidget>(*picker,"hueSaturation");requireControl<QWidget>(*picker,"brightness");
    for(auto* swatch:picker->findChildren<QPushButton*>()) {
        require(swatch->size()==QSize(21,19),"Global button style enlarged a compact color swatch");
        require(picker->rect().contains(QRect(swatch->mapTo(picker,QPoint(0,0)),swatch->size())),"Basic color grid is clipped inside the compact picker");
    }
    auto* firstSwatch=requireControl<QPushButton>(*picker,"basicColor.0");
    auto* lastInFirstRow=requireControl<QPushButton>(*picker,"basicColor.7");
    auto* firstInSecondRow=requireControl<QPushButton>(*picker,"basicColor.8");
    require(firstSwatch->y()==lastInFirstRow->y() && firstInSecondRow->y()>firstSwatch->y(),"Basic colors must have eight columns and six rows");

    color->setText("18A7C3");
    require(picker->currentColor()==QColor("#18A7C3"),"Hex edits must synchronize the full-range picker");
    color->setText("18");require(color->text()=="18","Picker overwrote an incomplete hex edit");
    picker->setCurrentColor(QColor("#203040"));
    require(!QApplication::activeModalWidget(),"Inline editing opened a modal window");
    if(!qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES").isEmpty()) {
        window.resize(1180,1250);QApplication::processEvents();
        for(auto* parent=picker->parentWidget();parent;parent=parent->parentWidget())
            if(auto* scroll=qobject_cast<QScrollArea*>(parent)) {
                scroll->ensureWidgetVisible(picker);QApplication::processEvents();break;
            }
        window.grab().save(qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES")+"/subtitle-color-picker.png");
    }
    choose->click();QApplication::processEvents();
    require(!picker->isVisible(),"Inline color picker did not collapse");
    require(color->text()=="203040","Inline color selection failed");
    auto* headroom=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.minimum_headroom_pixels");
    require(headroom->text()=="12","Moved subtitle headroom default missing");headroom->setText("24");
    blur->setText("30");
    requireControl<QLineEdit>(window,"config.vprenderer.subtitles.subtitle_generated_gray_border_width")->setText("2");
    list->setCurrentRow(0);
    auto* hold=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.subtitle_hold_seconds");
    require(hold->isVisible() && !blur->isVisible(),"Classic/reconstruction visibility incorrect");
    require(requireControl<QLineEdit>(window,"config.vprenderer.subtitles.subtitle_engage_drift_ms")->text()=="500","Classic engage default missing");
    require(requireControl<QLineEdit>(window,"config.vprenderer.subtitles.subtitle_release_drift_ms")->text()=="2000","Classic release default missing");
    hold->setText("1.5");
    requireControl<QLineEdit>(window,"config.vprenderer.subtitles.subtitle_engage_drift_ms")->setText("500");
    list->setCurrentRow(1);
    require(picker->currentColor()==QColor("#203040"),"Profile reload did not synchronize picker");
    auto* borderChoose=requireControl<QPushButton>(window,"config.vprenderer.subtitles.subtitle_generated_gray_border_color.picker.toggle");
    borderChoose->click();
    auto* borderPicker=dynamic_cast<InlineColorEditor*>(requireControl<QWidget>(window,"config.vprenderer.subtitles.subtitle_generated_gray_border_color.picker.editor"));
    require(borderPicker!=nullptr,"Compact border editor missing");
    borderPicker->setCurrentColor(QColor("#6C2E91"));borderChoose->click();
    require(color->text()=="203040","Border color changed background color");

    require(!window.findChild<QCheckBox*>("config.vprenderer.zoom.subtitle_fit"),"Legacy subtitle control remains on Zoom");
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, {}, false, {}, {}, {}, "vprenderer.subtitles.gray");
    type->setCurrentIndex(type->findData("black"));offset->setText("22");
    require(color->isVisible() && !blur->isVisible(),"Solid mode must show shared appearance without reconstruction controls");
    require(color->text()=="203040","Switching styles discarded background color");
    auto checkVisibleRows=[&] {
        QApplication::processEvents();
        for(auto* field:window.findChildren<QLineEdit*>())
            if(field->objectName().startsWith("config.vprenderer.subtitles.") && field->isVisible())
            {
                require(field->parentWidget()->rect().contains(field->geometry()),"Subtitle control clipped by its row after style switch");
                for(auto* parent=field->parentWidget();parent;parent=parent->parentWidget())
                    if(parent->property("card").toBool()) {
                        require(parent->rect().contains(QRect(field->mapTo(parent,QPoint(0,0)),field->size())),"Subtitle control clipped by profile card after style switch");
                        break;
                    }
            }
    };
    checkVisibleRows();
    type->setCurrentIndex(2);checkVisibleRows();
    type->setCurrentIndex(3);checkVisibleRows();


    if(!qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES").isEmpty()) {
        window.resize(1180,1250); QApplication::processEvents();
        window.grab().save(qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES")+"/subtitle-solid.png");
        type->setCurrentIndex(2); QApplication::processEvents();
        auto* brightness=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.subtitle_generated_gray_max_luminance");
        for(auto* parent=brightness->parentWidget();parent;parent=parent->parentWidget())
            if(auto* scroll=qobject_cast<QScrollArea*>(parent)) {
                scroll->ensureWidgetVisible(brightness); QApplication::processEvents();
                require(scroll->viewport()->rect().contains(QRect(brightness->mapTo(scroll->viewport(),QPoint(0,0)),brightness->size())),"Reconstruction controls cannot be reached by scrolling");
                break;
            }
        window.grab().save(qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES")+"/subtitle-reconstructed.png");
        list->setCurrentRow(0); QApplication::processEvents();
        window.grab().save(qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES")+"/subtitle-classic.png");
        list->setCurrentRow(1); type->setCurrentIndex(3);
    }
    save(window);
    ConfigEditorWindow reopened(path,0,true);
    requireControl<QListWidget>(reopened,"config.vprenderer.subtitles.profiles")->setCurrentRow(1);
    require(requireControl<QComboBox>(reopened,"config.vprenderer.subtitles.type")->currentData().toString()=="black","Subtitle style not saved");
    require(requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.offset_pixels")->text()=="22","Subtitle offset not saved");
    require(requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.subtitle_text_color")->text()=="B0B0B0","Text color not saved");
    require(requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.subtitle_generated_gray_color")->text()=="203040","Solid color not saved");
    require(requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.subtitle_generated_gray_border_color")->text()=="6C2E91","Arbitrary border color not saved");
    require(requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.minimum_headroom_pixels")->text()=="24","Headroom not saved");
    require(requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.subtitle_generated_gray_border_width")->text()=="2","Border not saved");
    requireControl<QComboBox>(reopened,"config.vprenderer.subtitles.type")->setCurrentIndex(2);
    require(requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.subtitle_generated_gray_blur_px")->text()=="30","Switching modes lost blur setting");
    requireControl<QListWidget>(reopened,"config.vprenderer.subtitles.profiles")->setCurrentRow(0);
    require(requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.subtitle_hold_seconds")->text()=="1.5","Classic hold not saved");

}

void testSubtitleBackgroundShapeRoundTripAndVisibility()
{
    QTemporaryDir dir;const QString path=dir.filePath("VideoProcessor.cfg");
    QFile file(path);require(file.open(QIODevice::WriteOnly),"Cannot write subtitle shape fixture");
    // Avoid the unrelated refresh-rate migration, which intentionally marks old configs dirty.
    file.write("[general]\nrenderer: VP Renderer\nswitch_refresh_rate: fullscreen_only\n[vprenderer.subtitles.Moved]\ntype: generated_gray\n");file.close();
    ConfigEditorWindow window(path,0,true);window.show();window.selectPage(19);QApplication::processEvents();
    auto* type=requireControl<QComboBox>(window,"config.vprenderer.subtitles.type");
    auto* rounded=requireControl<QCheckBox>(window,"config.vprenderer.subtitles.rounded_corners");
    auto* edge=requireControl<QComboBox>(window,"config.vprenderer.subtitles.background_edge");
    require(rounded->isChecked() && edge->currentData().toString()=="extend","Shape defaults changed existing rounded extended behavior");
    require(!requireControl<QPushButton>(window,"applyConfiguration")->isEnabled(),"Loading shape defaults dirtied configuration");
    rounded->setChecked(false);selectData(edge,"float");
    for(const auto& style:{"black","classic","off","generated_gray"}) {
        selectData(type,style);QApplication::processEvents();
        const bool moved=QString(style)=="black" || QString(style)=="generated_gray";
        require(rounded->isVisible()==moved && edge->isVisible()==moved,"Shape controls must only show for moved styles");
        require(!rounded->isChecked() && edge->currentData().toString()=="float","Style switch lost shape selection");
    }
    selectData(type,"black");save(window);
    ConfigEditorWindow reopened(path,0,true);
    require(!requireControl<QCheckBox>(reopened,"config.vprenderer.subtitles.rounded_corners")->isChecked(),"Square corners did not persist");
    require(requireControl<QComboBox>(reopened,"config.vprenderer.subtitles.background_edge")->currentData().toString()=="float","Float did not persist");
    requireControl<QCheckBox>(reopened,"config.vprenderer.subtitles.rounded_corners")->setChecked(true);
    selectData(requireControl<QComboBox>(reopened,"config.vprenderer.subtitles.background_edge"),"extend");save(reopened);
    ConfigEditorWindow restored(path,0,true);
    require(requireControl<QCheckBox>(restored,"config.vprenderer.subtitles.rounded_corners")->isChecked() && requireControl<QComboBox>(restored,"config.vprenderer.subtitles.background_edge")->currentData().toString()=="extend","Restored shape did not persist");
}

void testSubtitleTextSizeReductionRoundTripAndStyleVisibility()
{
    QTemporaryDir dir;
    const QString path=dir.filePath("VideoProcessor.cfg");
    QFile file(path); require(file.open(QIODevice::WriteOnly),"Cannot write subtitle text size fixture");
    file.write("[general]\nrenderer: VP Renderer\n"
        "[vprenderer.subtitles.Moved]\ntype: generated_gray\n");file.close();
    ConfigEditorWindow window(path,0,true);
    window.show();
    requireControl<QStackedWidget>(window,"settingsPages")->setCurrentIndex(19);
    QApplication::processEvents();
    auto* type=requireControl<QComboBox>(window,"config.vprenderer.subtitles.type");
    auto* reduction=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.text_reduction_percent");
    require(reduction->isVisible() && reduction->text()=="0",
        "Moved text size must default to no reduction");
    require(reduction->toolTip().contains("0% keeps") && reduction->toolTip().contains("25%"),
        "Text size help must explain unchanged and minimum sizes");
    for(const auto& input:{QStringLiteral("-1"),QStringLiteral("76"),QStringLiteral("12.5")}) {
        reduction->setText(input);
        require(!reduction->hasAcceptableInput(),"Text size accepts a value outside integer 0-75 percent");
    }
    reduction->setText("75");
    require(reduction->hasAcceptableInput(),"Text size rejects the 75 percent reduction endpoint");
    for(const auto& style:{QStringLiteral("black"),QStringLiteral("classic"),QStringLiteral("off"),QStringLiteral("generated_gray")}) {
        type->setCurrentIndex(type->findData(style));QApplication::processEvents();
        const bool moved=style=="black" || style=="generated_gray";
        require(reduction->isVisible()==moved,"Text size visibility does not match moved subtitle styles");
        require(reduction->text()=="75","Switching subtitle styles discarded text size");
    }
    type->setCurrentIndex(type->findData("black"));
    save(window);
    ConfigEditorWindow reopened(path,0,true);
    auto* reopenedType=requireControl<QComboBox>(reopened,"config.vprenderer.subtitles.type");
    auto* reopenedReduction=requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.text_reduction_percent");
    require(reopenedType->currentData().toString()=="black" && reopenedReduction->text()=="75",
        "Solid text size reduction did not survive save/reopen");
    reopenedType->setCurrentIndex(reopenedType->findData("generated_gray"));
    reopenedReduction->setText("0");
    require(reopenedReduction->hasAcceptableInput(),"Text size rejects no reduction");
    save(reopened);
    ConfigEditorWindow unchanged(path,0,true);
    require(requireControl<QComboBox>(unchanged,"config.vprenderer.subtitles.type")->currentData().toString()=="generated_gray" &&
        requireControl<QLineEdit>(unchanged,"config.vprenderer.subtitles.text_reduction_percent")->text()=="0",
        "Explicit zero text size reduction did not survive save/reopen");
}

void testSubtitleHdrLegacyFallbackAndSharedModes()
{
    QTemporaryDir dir;const QString path=dir.filePath("VideoProcessor.cfg");
    const QByteArray fixture="[general]\nrenderer: VP Renderer\nswitch_refresh_rate: fullscreen_only\n"
        "[vprenderer.zoom.First]\nautomatic_crop: true\nhdr_peak_analysis_picture_only: true\n"
        "hdr_peak_analysis_motion_compensation: false\nhdr_peak_analysis_height_percent: 68\n"
        "hdr_peak_analysis_position: bottom\n# keep the legacy owner until edited\n"
        "[vprenderer.zoom.Second]\nhdr_peak_analysis_picture_only: false\nhdr_peak_analysis_motion_compensation: true\n"
        "hdr_peak_analysis_height_percent: 80\nhdr_peak_analysis_position: top\n"
        "[vprenderer.subtitles.First]\ntype: classic\n";
    QFile file(path);require(file.open(QIODevice::WriteOnly),"Cannot write HDR fallback fixture");file.write(fixture);file.close();
    ConfigEditorWindow window(path,0,true);window.show();window.selectPage(19);QApplication::processEvents();
    auto* section=requireControl<QToolButton>(window,"subtitleSection.hdr_analysis");section->click();
    auto* mode=requireControl<QComboBox>(window,"config.vprenderer.subtitles.hdr_peak_analysis_mode");
    auto* height=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.hdr_peak_analysis_height_percent");
    auto* position=requireControl<QComboBox>(window,"config.vprenderer.subtitles.hdr_peak_analysis_position");
    require(mode->currentData().toString()=="fixed" && height->text()=="68" && position->currentData().toString()=="bottom","Subtitle controls did not preserve legacy HDR settings");
    require(!requireControl<QPushButton>(window,"applyConfiguration")->isEnabled(),"Loading inherited HDR settings dirtied the profile");
    require(readBytes(path)==fixture,"Merely opening Subtitle HDR controls rewrote the old configuration");
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, {}, false,"vprenderer.zoom.Second",{}, {},"vprenderer.subtitles.First");
    require(mode->currentData().toString()=="automatic" && height->text()=="80" && position->currentData().toString()=="top","Live Zoom switch did not refresh inherited Subtitle HDR values");
    require(!requireControl<QPushButton>(window,"applyConfiguration")->isEnabled(),"Refreshing inherited HDR settings wrote overrides");
    selectData(mode,"fixed");height->setText("77");
    auto* offset=requireControl<QLineEdit>(window,"config.vprenderer.subtitles.offset_pixels");offset->setText("29");
    window.setActiveProfileStatusForTesting({}, {}, {}, {}, {}, false,"vprenderer.zoom.First",{}, {},"vprenderer.subtitles.First");
    require(mode->currentData().toString()=="fixed" && height->text()=="77" && offset->text()=="29","Live fallback refresh overwrote pending Subtitle edits");
    require(position->currentData().toString()=="bottom","Still-inherited HDR field did not follow changed legacy owner");
    auto* type=requireControl<QComboBox>(window,"config.vprenderer.subtitles.type");
    for(const auto& style:{"classic","generated_gray","black"}) {
        selectData(type,style);QApplication::processEvents();
        require(mode->isVisible() && height->isVisible() && mode->currentData().toString()=="fixed","HDR protection became Classic-only or was reset by switching subtitle style");
    }
    selectData(mode,"automatic");save(window);
    const auto saved=readBytes(path);
    require(saved.contains("# keep the legacy owner until edited") && saved.contains("hdr_peak_analysis_height_percent: 68"),"Legacy values/comments lost");
    ConfigEditorWindow reopened(path,0,true);
    require(requireControl<QComboBox>(reopened,"config.vprenderer.subtitles.hdr_peak_analysis_mode")->currentData().toString()=="automatic","Subtitle Smart override did not survive a legacy Percentage setting");
    require(!requireControl<QLineEdit>(reopened,"config.vprenderer.subtitles.hdr_peak_analysis_height_percent")->isEnabled(),"Percentage controls enabled during Smart mode");
}

void testMotionAndChromaControlsPersistAndExposeNewTab()
{
    QTemporaryDir directory;
    const QString path=copyFixture(directory);
    QFile file(path); require(file.open(QIODevice::Append),"Cannot add Motion fixture");
    file.write("\n[vprenderer.motion]\nsmooth_motion: off\nframe_mixer: oversample\ndeinterlace: auto\ndeinterlace_algorithm: bwdif\nfield_order: auto\ndeinterlace_cadence: field\ndeinterlace_skip_spatial: false\n");file.close();
    ConfigEditorWindow window(path,0,true);window.selectPage(22);window.show();QApplication::processEvents();
    auto* tabs=requireControl<QTabBar>(window,"settingsSectionTabs");
    require(tabs->count()==8&&tabs->tabText(6)=="Motion"&&tabs->currentIndex()==6,"Motion tab is missing or not selected");
    auto* mode=requireControl<QComboBox>(window,"config.vprenderer.motion.smooth_motion");
    auto* mixer=requireControl<QComboBox>(window,"config.vprenderer.motion.frame_mixer");
    require(mode->currentData()=="off"&&mixer->currentData()=="oversample","Motion defaults changed");
    require(requireControl<QLabel>(window,"motionSection.smoothMotion")->isVisible(),"Smooth motion section is not expanded");
    require(requireControl<QLabel>(window,"motionSection.deinterlacing")->isVisible(),"Deinterlacing section is not expanded");
    auto* advanced=requireControl<QToolButton>(window,"rendererSection.deinterlacingAdvanced");
    require(!advanced->isChecked(),"Advanced deinterlacing should start collapsed");
    advanced->setChecked(true);
    for(const char* key:{"smooth_motion","frame_mixer","deinterlace","deinterlace_algorithm","field_order","deinterlace_cadence","deinterlace_skip_spatial"}) {
        auto* control=requireControl<QComboBox>(window,QString("config.vprenderer.motion.")+key);
        require(!control->toolTip().trimmed().isEmpty(),"Motion control tooltip is blank");
    }
    require(mixer->toolTip().contains("Applies only to smooth motion"),"Mixer relationship is not explained");
    // Hunt 2 (08 #2): libplacebo's mixer clips BTB; the tooltip names VP's motion as the BTB-safe path.
    require(mode->toolTip().contains("Below-black")&&mode->toolTip().contains("VP's own motion engine"),
        "Smooth motion tooltip does not explain the below-black limitation");

    selectData(mode,"always");selectData(mixer,"cubic");
    selectData(requireControl<QComboBox>(window,"config.vprenderer.motion.field_order"),"bottom");
    selectData(requireControl<QComboBox>(window,"config.vprenderer.motion.deinterlace_algorithm"),"yadif");
    selectData(requireControl<QComboBox>(window,"config.vprenderer.motion.deinterlace_cadence"),"frame");
    selectData(requireControl<QComboBox>(window,"config.vprenderer.motion.deinterlace_skip_spatial"),"true");
    window.selectPage(17);
    auto* chroma=requireControl<QComboBox>(window,"config.vprenderer.scaling.chroma_upscaler");
    require(chroma->findData("auto")==-1,"Chroma unexpectedly exposes Auto");
    selectData(chroma,"lanczos");save(window);
    ConfigEditorWindow reopened(path,0,true);reopened.selectPage(22);
    require(requireControl<QComboBox>(reopened,"config.vprenderer.motion.smooth_motion")->currentData()=="always","Smooth motion did not persist");
    require(requireControl<QComboBox>(reopened,"config.vprenderer.motion.frame_mixer")->currentData()=="cubic","Mixer did not persist");
    reopened.selectPage(17);require(requireControl<QComboBox>(reopened,"config.vprenderer.scaling.chroma_upscaler")->currentData()=="lanczos","Chroma did not persist");
    const QString images=qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES");
    if(!images.isEmpty()){reopened.resize(1040,700);reopened.show();reopened.selectPage(22);QApplication::processEvents();reopened.grab().save(images+"/motion.png");reopened.selectPage(17);QApplication::processEvents();reopened.grab().save(images+"/chroma.png");}
}

// Audit 2026-10-17: confirmed Config editor bugs before a deploy.
void writeFixture(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot write fixture");
    require(file.write(contents) == contents.size(), "Cannot write fixture");
}

void testNamedProfileFallsBackToBuiltInDefault()
{
    // Bug 1: a named profile whose default profile leaves a setting unset shows
    // VP's built-in default (rounded corners and dynamic target are on).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\nquality: high\n[vprenderer.Other]\nquality: medium\n"
        "[vprenderer.subtitles.Classic]\ntype: classic\n[vprenderer.subtitles.Black]\ntype: black\n");
    ConfigEditorWindow window(path, 0, true);
    for (int row : { 0, 1 })
    {
        requireControl<QListWidget>(window, "config.vprenderer.subtitles.profiles")->setCurrentRow(row);
        requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(row);
        QCoreApplication::processEvents();
        require(requireControl<QCheckBox>(window, "config.vprenderer.subtitles.rounded_corners")->isChecked(),
            "Rounded corners does not show its built-in default (on)");
        require(requireControl<QCheckBox>(window, "config.vprenderer.dynamic_target")->isChecked(),
            "Dynamic target does not show its built-in default (on)");
    }
    require(!requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(), "Showing defaults made edits");
}

void testRevealReloadsAnOutOfBandEdit()
{
    // Bug 2: Config hidden in the tray keeps its document; a reveal reloads a
    // clean editor whose file changed meanwhile.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\nprofile_change_display_seconds: 5\n[vprenderer.Cinema]\nquality: high\n");
    ConfigEditorWindow window(path, 0, true);
    require(requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds")->value() == 5, "fixture");
    window.hide();
    writeFixture(path, "[general]\nprofile_change_display_seconds: 9\n[vprenderer.Cinema]\nquality: high\n");
    window.reveal();
    QCoreApplication::processEvents();
    require(requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds")->value() == 9,
        "A reveal did not reload the changed file");
    require(!requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(), "The reload left edits");
    window.hide();
}

void testSaveAsksWhenTheFileChangedOnDisk()
{
    // Bug 2: a dirty editor whose file changed on disk asks Reload / Overwrite
    // / Cancel instead of overwriting the other change silently.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    const QByteArray original = "[general]\nprofile_change_display_seconds: 5\n[vprenderer.Cinema]\nquality: high\n";
    const QByteArray external = "# hand edit\n[general]\nprofile_change_display_seconds: 7\n[vprenderer.Cinema]\nquality: high\n";
    writeFixture(path, original);
    ConfigEditorWindow window(path, 0, true);
    window.setPromptsForTesting(true);
    auto* seconds = requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds");
    seconds->setValue(3);
    writeFixture(path, external);
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    // Cancel: nothing written, the edit kept.
    answerMessageBox(QMessageBox::Cancel);
    apply->click();
    QCoreApplication::processEvents();
    require(readBytes(path) == external, "Cancel overwrote the changed file");
    require(apply->isEnabled(), "Cancel lost the edit");
    // A reveal keeps a dirty editor's edits.
    window.reveal();
    QCoreApplication::processEvents();
    require(requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds")->value() == 3,
        "A reveal discarded unsaved edits");
    window.hide();
    // Overwrite: the edits are written.
    answerMessageBox(QMessageBox::Save);
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("profile_change_display_seconds: 3"), "Overwrite did not save the edit");
    // Hunt 2 (L2): the replaced external edit is kept; no migration backup is made.
    const QStringList overwriteBackups = QDir(dir.path()).entryList(
        { QStringLiteral("VideoProcessor.cfg.before-overwrite.*.bak") }, QDir::Files);
    require(overwriteBackups.size() == 1 && readBytes(dir.filePath(overwriteBackups.front())) == external,
        "Overwrite did not keep the external edit in a .before-overwrite backup");
    require(QDir(dir.path()).entryList({ QStringLiteral("*.before-config-migration.*.bak") }, QDir::Files).isEmpty(),
        "A plain overwrite made a migration backup");
    // Reload: edits discarded, the file's content shown.
    writeFixture(path, external);
    requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds")->setValue(2);
    answerMessageBox(QMessageBox::Reset);
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    require(readBytes(path) == external, "Reload wrote the file");
    require(requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds")->value() == 7,
        "Reload did not show the file's value");
}

void testCancelRunsEveryLoadMigration()
{
    // Bug 3: Cancel reloads with the same migrations as opening Config (the
    // refresh-rate switch's legacy true reads as Full Screen Only).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\nswitch_refresh_rate: true\n[vprenderer.Cinema]\nquality: high\n");
    ConfigEditorWindow window(path, 0, true);
    require(requireControl<QComboBox>(window, "config.general.switch_refresh_rate")->currentData().toString() == "fullscreen_only",
        "Opening Config did not migrate switch_refresh_rate");
    requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds")->setValue(9);
    requireControl<QPushButton>(window, "cancelConfiguration")->click();
    QCoreApplication::processEvents();
    require(requireControl<QComboBox>(window, "config.general.switch_refresh_rate")->currentData().toString() == "fullscreen_only",
        "Cancel reloaded without the refresh-rate migration");
    require(readBytes(path).contains("switch_refresh_rate: true"), "Cancel wrote the file");
}

void testClearedChoiceShowsTheInheritedValue()
{
    // Bug 4 and suffix (e): "Not set" shows what then applies - the default
    // profile's value "(inherited)", or VP's own "(built-in default)".
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\noptical_flow: off\n[vprenderer.Other]\noptical_flow: on\n"
        "[vprenderer.Third]\ndejudder_fade_match: off\n");
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    auto* engine = requireControl<QRadioButton>(window, "config.vprenderer.optical_flow.engine");
    auto* matcher = requireControl<QRadioButton>(window, "config.vprenderer.optical_flow.matcher");
    auto* unset = requireControl<QToolButton>(window, "config.vprenderer.optical_flow.unset");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(engine->isChecked(), "fixture: the profile's own engine choice");
    // Organisation pass 2: optical_flow is the Motion tier's key, greyed until Custom.
    requireControl<QCheckBox>(window, "vpv.motion.card.tier_keys.custom")->setChecked(true);
    QCoreApplication::processEvents();
    unset->click();
    QCoreApplication::processEvents();
    require(matcher->isChecked() && !engine->isChecked(), "Not set still shows the profile's old value");
    require(matcher->text().contains("(inherited)"), (QByteArray("not marked inherited: ") + matcher->text().toUtf8()).constData());
    // The fade match: the default profile does not set it, so VP's own default.
    auto* fade = requireControl<QComboBox>(window, "config.vprenderer.dejudder_fade_match");
    profiles->setCurrentRow(2);
    QCoreApplication::processEvents();
    selectData(fade, "");
    require(fade->property("inheritedValue").toString().compare("on", Qt::CaseInsensitive) == 0,
        "The cleared fade match does not hold VP's default (on)");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(engine->isChecked() == false && matcher->isChecked(), "fixture: inherited matcher");
    profiles->setCurrentRow(2);
    QCoreApplication::processEvents();
    require(matcher->text().contains("(inherited)"), "an inherited choice is not marked inherited");
    // A setting no profile sets: built-in default.
    QTemporaryDir dir2; const QString path2 = dir2.filePath("VideoProcessor.cfg");
    writeFixture(path2, "[vprenderer.Cinema]\nquality: high\n[vprenderer.Other]\noptical_flow: off\n");
    ConfigEditorWindow second(path2, 0, true);
    requireControl<QListWidget>(second, "config.vprenderer.profiles")->setCurrentRow(1);
    QCoreApplication::processEvents();
    requireControl<QCheckBox>(second, "vpv.motion.card.tier_keys.custom")->setChecked(true);
    QCoreApplication::processEvents();
    requireControl<QToolButton>(second, "config.vprenderer.optical_flow.unset")->click();
    QCoreApplication::processEvents();
    auto* secondEngine = requireControl<QRadioButton>(second, "config.vprenderer.optical_flow.engine");
    require(secondEngine->isChecked() && secondEngine->text().contains("(built-in default)"),
        (QByteArray("VP's default is not marked built-in: ") + secondEngine->text().toUtf8()).constData());
}

void testUtf8BomAndEqualsSeparator()
{
    // Bugs 5 and 7: a UTF-8 BOM is read past and written back; SectionSettings
    // splits on ':' or '=' like VP's reader.
    ConfigEditorCore::ConfigDocument document;
    document.LoadBytes(L"memory", std::string("\xEF\xBB\xBF[general]\r\nrenderer = VP Renderer\r\n"));
    require(document.hasUtf8Bom && document.lines.front() == "[general]", "The BOM was not stripped on load");
    const auto settings = document.SectionSettings("general");
    require(settings.size() == 1 && settings.front().first == "renderer" && settings.front().second == "VP Renderer",
        "SectionSettings did not split on '='");
    require(document.Serialize().rfind("\xEF\xBB\xBF[general]", 0) == 0, "The BOM was not written back");
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "\xEF\xBB\xBF[general]\nprofile_change_display_seconds: 5\n[vprenderer.Cinema]\nquality: high\n");
    {
        ConfigEditorWindow window(path, 0, true);
        auto* seconds = requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds");
        require(seconds->value() == 5, "The first section after a BOM was not read");
        seconds->setValue(6);
        save(window);
    }
    const QByteArray saved = readBytes(path);
    require(saved.startsWith("\xEF\xBB\xBF[general]") && saved.contains("profile_change_display_seconds: 6") &&
        saved.count("[general]") == 1, (QByteArray("The BOM file did not round-trip: ") + saved.left(200)).constData());
}

void testLoadWritesNothingUnrequested()
{
    // Bug 6: the sample opens with Apply disabled; absent keys and the Motion
    // starting profile are not written; a real migration keeps a backup.
    {
        QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
        require(QFile::copy(repositoryPath(QStringLiteral("VideoProcessor.cfg")), path), "Cannot copy the sample");
        const QByteArray sample = readBytes(path);
        ConfigEditorWindow window(path, 0, true);
        require(readBytes(path) == sample, "Opening the sample wrote it");
        if (requireControl<QPushButton>(window, "applyConfiguration")->isEnabled())
        {
            // Say which lines the load-time migration changed.
            save(window);
            const QList<QByteArray> before = sample.split('\n'), after = readBytes(path).split('\n');
            QByteArray changed;
            for (const QByteArray& line : after)
                if (!before.contains(line)) changed += "+" + line.trimmed() + " ";
            for (const QByteArray& line : before)
                if (!after.contains(line)) changed += "-" + line.trimmed() + " ";
            require(false, ("The sample configuration opens with unsaved changes (a load-time migration): " +
                changed.left(600)).constData());
        }
    }
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\nprofile_change_display_seconds: 5\n[vprenderer.Cinema]\nquality: high\n");
    {
        ConfigEditorWindow window(path, 0, true);
        require(!requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(), "A minimal file opens dirty");
        require(requireControl<QComboBox>(window, "config.general.switch_refresh_rate")->currentData().toString() == "fullscreen_only",
            "An absent switch_refresh_rate does not show VP's default");
        requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds")->setValue(6);
        save(window);
    }
    const QByteArray saved = readBytes(path);
    require(!saved.contains("switch_refresh_rate") && !saved.contains("vprenderer.motion"),
        (QByteArray("Unrequested keys were written: ") + saved).constData());
    require(QDir(dir.path()).entryList({ QStringLiteral("*.bak") }, QDir::Files).isEmpty(), "A plain edit made a backup");
    // A real migration (legacy true) keeps the original file.
    writeFixture(path, "[general]\nswitch_refresh_rate: true\n[vprenderer.Cinema]\nquality: high\n");
    {
        ConfigEditorWindow window(path, 0, true);
        save(window);
    }
    require(readBytes(path).contains("switch_refresh_rate: fullscreen_only"), "The migration was not saved");
    require(QDir(dir.path()).entryList({ QStringLiteral("*.bak") }, QDir::Files).size() == 1,
        "A migration did not keep a backup of the original file");
}

// Owner rule (every setting has a control), schema-driven (integration 2026-10-17;
// replaces the hard-coded coverage list): every key of the LED and projector
// schemas (LedDriveRules / ProjectorRules), of the [general] application schema
// (MainConfigSchema::GeneralRules) and every renderer key RendererProfileConfig
// validates has a Config control - a widget whose object name ends in ".<key>" -
// or is on the explicit allow-list below, each entry with its reason.
static std::set<std::string> RendererValidatedKeys()
{
    // RendererProfileConfig validates with if-chains; its keys are read from the
    // validators' source (ValidateProfileSetting and ValidateBaseSetting).
    std::string path = __FILE__;
    std::replace(path.begin(), path.end(), '/', '\\');
    path.resize(path.rfind("\\src\\") + 5);
    std::ifstream file(path + "VideoProcessor-Lib\\RendererProfileConfig.h", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::set<std::string> keys;
    for (const char* function : { "inline bool ValidateProfileSetting(", "inline bool ValidateBaseSetting(" })
    {
        const size_t start = text.find(function);
        if (start == std::string::npos) continue;
        const size_t end = text.find("\n\tinline ", start + 10);
        const std::string body = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        // Any spacing: `key == "x"`, `key=="x"` (hunt 2: the compact spelling hid
        // subtitle_detection_optimization).
        static const std::regex marker(R"re(\bkey\s*==\s*"([^"]+)")re");
        for (std::sregex_iterator it(body.begin(), body.end(), marker), last; it != last; ++it)
            keys.insert((*it)[1].str());
    }
    return keys;
}

// The schema sources the coverage test (and the settings inventory, --inventory) walk.
struct CoverageSource { const char* name; std::vector<std::string> sections; std::vector<std::string> keys; };
static std::vector<CoverageSource> CoverageSources()
{
    auto keysOf = [](const std::vector<ConfigSchema::KeyRule>& rules)
    {
        std::vector<std::string> keys;
        for (const auto& rule : rules) keys.push_back(rule.key);
        return keys;
    };
    const std::set<std::string> rendererKeySet = RendererValidatedKeys();
    std::vector<std::string> toneKeys;
    for (const auto& spec : ToneMappingTuning::Specs())
        if (!rendererKeySet.count(spec.key)) toneKeys.push_back(spec.key);
    std::vector<std::string> retiredKeys;
    for (const char* key : { "fix_scene_cooldown", "dejudder_picture_area", "dither_vectors_new_frame" })
        if (RetiredSettings::FindKey(key) && !rendererKeySet.count(key)) retiredKeys.push_back(key);
    return {
        { "led_drive", { "led_drive" }, keysOf(LedDrive::LedDriveRules()) },
        { "projector", { "projector" }, keysOf(LedDrive::ProjectorRules()) },
        { "general", { "general", "command_line" }, keysOf(MainConfigSchema::GeneralRules()) },
        { "general", { "general" }, { MainConfigSchema::BoundedInvalidCaptureRecoveryRule().key } },
        { "queue", { "queue" }, keysOf(MainConfigSchema::QueueRules()) },
        { "queue_recovery", { "queue_recovery", "queue" }, keysOf(MainConfigSchema::RecoveryRules()) },
        { "directshow", { "directshow" }, keysOf(MainConfigSchema::DirectShowRules()) },
        { "vprenderer.input_processing", { "vprenderer.input_processing" }, keysOf(MainConfigSchema::VpRendererInputRules()) },
        { "decklink", { "decklink" }, keysOf(MainConfigSchema::DeckLinkRules()) },
        { "directshow.conversion", { "directshow.conversion" }, keysOf(MainConfigSchema::ConversionRules()) },
        { "directshow.ppm", { "directshow.ppm" }, keysOf(MainConfigSchema::PpmRules()) },
        { "shortcuts", { "shortcuts" }, keysOf(MainConfigSchema::ShortcutSwitchRules()) },
        { "ambient", { "ambient" }, keysOf(Ambient::SchemaRules()) },
        // Automatic calibration: every [calibration] key has a control on the Calibration page.
        { "calibration", { "calibration" }, keysOf(Calibration::ConfigRules()) },
        { "lldv", { "lldv" }, keysOf(MainConfigSchema::LldvRules()) },
        { "logging", { "logging" }, keysOf(MainConfigSchema::LoggingRules()) },
        // RendererProfileConfig's validators also own the [queue.*] and [lldv.*] profile groups.
        { "renderer", { "vprenderer", "queue", "lldv" }, std::vector<std::string>(rendererKeySet.begin(), rendererKeySet.end()) },
        // Coverage audit 2026-10-24: the renderer also accepts keys through tables that the
        // `key == "x"` scan cannot see: the tone-mapping tuning table and the retired keys.
        { "renderer", { "vprenderer" }, toneKeys },
        { "renderer", { "vprenderer" }, retiredKeys },
    };
}

static const std::map<std::string, const char*>& CoverageAllowList()
{
    // Keys that intentionally have no control of their own, "<source>:<key>" with the
    // reason. Only retired, alias, legacy-location and diagnostic-only keys; a live
    // key never belongs here (hunt 2: subtitle_hold_ms / subtitle_near_bar_px were
    // listed as "legacy" while the Subtitles profiles read them).
    static const std::map<std::string, const char*> allowList = {
        // [led_drive]: the retired Python relay (removed 2026-10-15; accepted, ignored).
        { "led_drive:listen_port", "retired (ignored)" }, { "led_drive:listen_log", "retired (ignored)" },
        { "led_drive:python_drives_port", "retired (ignored)" }, { "led_drive:python_feed", "retired (ignored)" },
        { "led_drive:listen_script", "retired (ignored)" }, { "led_drive:python", "retired (ignored)" },
        // [led_drive]: superseded by the per-direction rate fields and the fade time.
        { "led_drive:rate_up", "alias: older single rate, superseded by rate_gain/min/max_up" },
        { "led_drive:rate_down", "alias: older single rate, superseded by rate_gain/min/max_down" },
        { "led_drive:fade_frames", "alias: older fade length in frames at 24p (fade_ms has the control)" },
        { "led_drive:fade_min_step", "alias: older per-frame fade slope, read as fade_min_slope x 24 (fade_min_slope has the control)" },
        { "led_drive:script_smoothing", "legacy: set from adaptive_gamma (which has the control) whenever command_source "
            "is named; read only from older mode-only files" },
        { "led_drive:mode", "legacy: older files' mode; Config shows it as command_source (which has the control) "
            "and keeps the older switches only for such files" },
        // [general]: spellings of keys that have a control under the other spelling.
        { "general:windowedfullscreenmode", "alias of windowed_fullscreen_mode" },
        { "general:no_ui", "alias of noui" }, { "general:start_minimized", "alias of startminimized" },
        { "general:new_lldv", "alias of newlldv" }, { "general:scene", "alias of scene_detect" },
        // [general]: kept for manual compatibility editing only (see the scene-detection control).
        { "general:disable_detection_features", "compatibility editing only" },
        { "general:scene_correction_mode", "compatibility editing only" },
        { "general:scene_correction_basic", "compatibility editing only" },
        // [general]: command-line / session state rather than settings.
        { "general:dolby_vision_tunnel", "command-line / per-session capture state" },
        { "general:interface", "command-line interface choice" },
        { "general:frame_offset", "legacy location: Config moves [general] frame_offset to [directshow] frame_offset, "
            "which has the control" },
        { "general:queue_size", "legacy location: VP reads [command_line] then [general] queue_size after the default "
            "queue profile (hunt 3, CFG9); Config moves both into [queue.*], which has the control" },
        { "general:renderer_start_stop_time_method", "legacy location: the [directshow] control replaces the [general] value on edit" },
        { "general:renderer_nominal_range", "legacy location: the [directshow] control replaces the [general] value on edit" },
        { "general:renderer_transfer_function", "legacy location: the [directshow] control replaces the [general] value on edit" },
        { "general:renderer_transfer_matrix", "legacy location: the [directshow] control replaces the [general] value on edit" },
        { "general:renderer_primaries", "legacy location: the [directshow] control replaces the [general] value on edit" },
        // Legacy queue locations Config folds into [queue.*] on load.
        { "queue:steady_reserve_frames", "alias: legacy spelling of target_frames; Config migrates it on load" },
        { "queue_recovery:reset_after_render_restart_seconds", "legacy location: Config moves it into [queue.*], which has the control" },
        { "queue_recovery:reset_queue_too_large_percent", "legacy location: Config moves it into [queue.*], which has the control" },
        { "directshow:presentation_lead_frames", "legacy location: Config moves it to [queue.*] lead_frames, which has the control" },
        // Dolby Vision profiles: edited by the DV mode buttons (config.dv.mode.lldv / .tunneling).
        { "lldv:dolby_vision_tunnel", "edited by the DV mode buttons (config.dv.mode.*)" },
        { "renderer:dolby_vision_tunnel", "edited by the DV mode buttons (config.dv.mode.*)" },
        { "general:subtitle_reposition", "command-line option (/subtitle_reposition); the [general] key is validated "
            "but never read at runtime" },
        // Renderer keys.
        { "renderer:deband", "legacy toggle; deband_strength has the control" },
        // RetiredSettings.h (pruning 2026-10-17): accepted with any value and ignored.
        { "renderer:fix_scene_cooldown", "retired (RetiredSettings.h): accepted with any value, ignored" },
        { "renderer:dejudder_picture_area", "retired (RetiredSettings.h): accepted with any value, ignored" },
        { "renderer:dither_vectors_new_frame", "retired (RetiredSettings.h): accepted with any value, ignored" },
        { "renderer:sdr_target_primaries", "legacy spelling of target_primaries" },
        { "renderer:calibration_lut_input_gamma", "retired: accepted but no longer controls LUT / HDR encoding "
            "(hdr_tone_map_target_gamma has the control)" },
        { "renderer:calibration_lut_input_transfer", "alias accepted for HDR target (Rendering)" },
        { "renderer:live_profile_updates", "alias: legacy switch for profile_update_mode live (which has the control)" },
        { "renderer:steady_reserve_frames", "alias: legacy spelling of target_frames; Config migrates it on load" },
        { "renderer:mode", "legacy Screen label (normal / scope): validated for compatibility, never changes the "
            "geometry; screen_aspect has the control" },
        { "renderer:profile_update_mode", "renderer policy read from [general]; the [general] control covers it" },
        { "renderer:switch_refresh_rate", "renderer policy read from [general]; Config migrates it there and the "
            "[general] control covers it" },
        { "renderer:subtitle_detection_optimization", "legacy subtitle key: accepted in renderer / Screen sections "
            "only, and the renderer resets it before applying the Subtitles profile (LVR, \"Discard legacy subtitle "
            "settings\"), so it never takes effect" },
        { "renderer:subtitle_bbox_test", "test / diagnostic overlay" },
        { "renderer:subtitle_cut_paste_test", "test / diagnostic overlay" },
        { "renderer:subtitle_cut_paste_background", "legacy subtitle key (rule-section background, migrated to "
            "the Subtitles profiles)" },
        { "renderer:subtitle_box_padding_mode", "legacy subtitle key (Subtitles profiles replace it)" },
        { "renderer:subtitle_box_padding_sides", "legacy subtitle key" },
        { "renderer:subtitle_box_padding_top", "legacy subtitle key" },
        { "renderer:subtitle_box_padding_bottom", "legacy subtitle key" },
        { "renderer:subtitle_box_padding_sides_percent", "legacy subtitle key" },
        { "renderer:subtitle_box_padding_top_percent", "legacy subtitle key" },
        { "renderer:subtitle_box_padding_bottom_percent", "legacy subtitle key" },
        { "renderer:subtitle_move_inset", "legacy subtitle key" },
        { "renderer:subtitle_padding_pixels", "legacy subtitle key" },
        { "renderer:subtitle_fit", "retired Screen setting (ignored)" },
        { "renderer:temporal_fill", "alias: legacy Subtitles switch for cleanup_fill temporal (cleanup_fill has the control)" },
        { "renderer:pipeline_original_pathway", "VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.pathway); also read from [vprenderer]" },
        { "renderer:pipeline_order", "VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.pipeline_order); also read from [vprenderer]" },
        { "renderer:pipeline_slots_off", "VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.pipeline_slots_off); also read from [vprenderer]" },
        { "renderer:calibration_lut_stage", "VP-V section (ADR-0023): stored in [vpv] by the VP-V page (vpv.expert.calibration_lut_stage); also read from [vprenderer]" },
        { "renderer:pipeline_entrance_once", "hidden diagnostic/test key (ADR-0024)" },
    };
    return allowList;
}

void testEverySchemaKeyHasAConfigControl()
{
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\nfullscreen: false\n[vprenderer]\nquality: high\n"
        "[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
        "[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
    ConfigEditorWindow window(path, 0, true);
    // Hunt 2 (2026-10-17): coverage is per section. A control named
    // "config.<section>.<key>" covers <key> only for the schema whose section it
    // belongs to, so a same-named control elsewhere (a shader's "curve", the
    // Actions page's "enabled") cannot hide a missing one.
    std::map<std::string, std::set<std::string>> controlSections; // key -> sections
    for (QObject* object : window.findChildren<QObject*>())
    {
        const QString name = object->objectName();
        if (!name.startsWith(QStringLiteral("config."))) continue;
        QStringList parts = name.mid(7).split(QLatin1Char('.'));
        // A key edited through several widgets names them "<key>.auto" / "<key>.value";
        // a combo's reset button is "<key>.unset", a unit label "<key>.unit".
        if (parts.size() > 2 && QStringList({ QStringLiteral("auto"), QStringLiteral("value"),
            QStringLiteral("unset"), QStringLiteral("unit") }).contains(parts.last()))
            parts.removeLast();
        if (parts.size() < 2) continue;
        const QString key = parts.takeLast();
        controlSections[key.toStdString()].insert(parts.join(QLatin1Char('.')).toStdString());
    }
    const std::vector<CoverageSource> sources = CoverageSources();
    require(RendererValidatedKeys().size() > 100, "the renderer validators' keys were not found");
    const std::map<std::string, const char*>& allowList = CoverageAllowList();

    std::set<std::string> usedAllowList;
    std::vector<std::string> missing;
    for (const CoverageSource& source : sources)
        for (const std::string& key : source.keys)
        {
            const auto found = controlSections.find(key);
            bool covered = false;
            std::string elsewhere;
            if (found != controlSections.end())
                for (const std::string& section : found->second)
                {
                    bool inSource = false;
                    for (const std::string& prefix : source.sections)
                        if (section == prefix || section.rfind(prefix + ".", 0) == 0) inSource = true;
                    if (inSource) covered = true;
                    else elsewhere += (elsewhere.empty() ? "" : ",") + section;
                }
            const std::string id = std::string(source.name) + ":" + key;
            if (allowList.count(id)) { usedAllowList.insert(id); continue; }
            if (!covered)
                missing.push_back(id + (elsewhere.empty() ? "" : " (only in " + elsewhere + ")"));
        }
    std::string list;
    for (const std::string& key : missing) list += " " + key;
    require(missing.empty(), ("Settings without a Config control (add one or allow-list it with a reason):" + list).c_str());
    // A stale allow-list entry (the key is no longer validated) would silently cover
    // a future key of the same name: keep the list exact.
    std::string stale;
    for (const auto& entry : allowList)
        if (!usedAllowList.count(entry.first)) stale += " " + entry.first;
    require(stale.empty(), ("Allow-list entries for keys no schema validates (remove them):" + stale).c_str());
}

// Settings coverage audit (docs/settings/coverage-audit.md; tools/settings_coverage_audit.py).
// `VideoProcessorConfigTests.exe --inventory <file>` writes, as tab-separated lines:
//   K <source> <sections> <key> <allow-list reason>   every schema key the coverage test walks;
//   C <object> <class> <shown> <tooltip?> <enabled> <tooltip>   every config. / vpv. control, with
//     what it shows when its key is unset (the default Config presents);
//   S <action> <chord>   the compiled shortcut defaults.
// It is not a test: it only reads, and exits 0 once the file is written.
static QString inventoryField(QString text)
{
    text.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '))
        .replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.trimmed();
}

int writeSettingsInventory(const QString& outPath)
{
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\nfullscreen: false\n[vprenderer]\nquality: high\n"
        "[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
        "[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
    ConfigEditorWindow window(path, 0, true);
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return 1;
    QTextStream stream(&out);
    stream.setEncoding(QStringConverter::Utf8);
    const auto& allowList = CoverageAllowList();
    for (const CoverageSource& source : CoverageSources())
    {
        QStringList sections;
        for (const std::string& s : source.sections) sections << QString::fromStdString(s);
        for (const std::string& key : source.keys)
        {
            const auto reason = allowList.find(std::string(source.name) + ":" + key);
            stream << "K\t" << source.name << '\t' << sections.join(QLatin1Char('|')) << '\t'
                << QString::fromStdString(key) << '\t'
                << (reason == allowList.end() ? QString() : inventoryField(QString::fromUtf8(reason->second))) << '\n';
        }
    }
    for (const char* key : { "pipeline_original_pathway", "pipeline_order", "pipeline_slots_off", "calibration_lut_stage" })
        stream << "K\tvpv\tvpv\t" << key << "\t\n";
    for (QWidget* w : window.findChildren<QWidget*>())
    {
        const QString name = w->objectName();
        if (!name.startsWith(QStringLiteral("config.")) && !name.startsWith(QStringLiteral("vpv."))) continue;
        QString shown;
        if (auto* combo = qobject_cast<QComboBox*>(w)) shown = combo->currentText();
        else if (auto* line = qobject_cast<QLineEdit*>(w)) shown = line->text().isEmpty() ? line->placeholderText() : line->text();
        else if (auto* spin = qobject_cast<QAbstractSpinBox*>(w)) shown = spin->text();
        else if (auto* slider = qobject_cast<QSlider*>(w)) shown = QString::number(slider->value());
        else if (auto* button = qobject_cast<QAbstractButton*>(w))
            shown = button->isCheckable() ? (button->isChecked() ? QStringLiteral("checked") : QStringLiteral("unchecked")) : QString();
        else if (auto* label = qobject_cast<QLabel*>(w)) shown = label->text().left(80);
        // Where it is: the page (createPage's viewport, "<title> settings") and the nearest tab,
        // else the nearest card (createCard: a "card" frame with a "cardTitle" label).
        QString page, tab, group;
        for (QWidget* parent = w->parentWidget(); parent; parent = parent->parentWidget())
        {
            // The innermost labelled group (organisation pass 2026-10-24): a collapsible section
            // ("<x>Section.<id>.content", titled by its toggle) or a card with a vpvCardTitle property.
            if (group.isEmpty())
            {
                const QString pn = parent->objectName();
                if (!parent->property("vpvCardTitle").toString().isEmpty())
                    group = parent->property("vpvCardTitle").toString();
                else if (pn.endsWith(QStringLiteral(".content")) && pn.contains(QStringLiteral("Section.")) && parent->parentWidget())
                {
                    if (auto* toggle = parent->parentWidget()->findChild<QToolButton*>(pn.chopped(8), Qt::FindDirectChildrenOnly))
                        group = toggle->text();
                }
            }
            if (tab.isEmpty())
            {
                if (auto* tabs = qobject_cast<QTabWidget*>(parent))
                {
                    for (int i = 0; i < tabs->count(); ++i)
                        if (tabs->widget(i) == w || tabs->widget(i)->isAncestorOf(w)) tab = tabs->tabText(i);
                }
                else if (parent->property("card").toBool())
                {
                    for (QLabel* title : parent->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly))
                        if (title->property("cardTitle").toBool()) { tab = title->text(); break; }
                }
            }
            if (parent->objectName() == QStringLiteral("pageViewport") && qobject_cast<QScrollArea*>(parent))
            {
                page = parent->accessibleName();
                if (page.endsWith(QStringLiteral(" settings"))) page.chop(9);
                break;
            }
        }
        stream << "C\t" << name << '\t' << w->metaObject()->className() << '\t' << inventoryField(shown) << '\t'
            << (w->toolTip().trimmed().isEmpty() ? 0 : 1) << '\t' << (w->isEnabled() ? 1 : 0) << '\t'
            << inventoryField(w->toolTip()).left(240) << '\t' << inventoryField(page) << '\t' << inventoryField(tab)
            << '\t' << inventoryField(group) << '\n';
    }
    for (const ShortcutDefault& d : kShortcutDefaults)
        stream << "S\t" << d.key << '\t' << d.chord << '\n';
    stream.flush();
    return stream.status() == QTextStream::Ok ? 0 : 1;
}

// `VideoProcessorConfigTests.exe --resave <file>` (tools/build_settings_preview.ps1): opens the file
// in a test-mode editor (no VP contact) and applies Config's own load-time migrations, so the
// settings preview's sandbox opens clean. Exit 0 = saved (or nothing to migrate), 1 = not saved.
// `VideoProcessorConfigTests.exe --screens <config> <folder>` (tools/settings_preview_screens.ps1, for
// the usability audit): opens a COPY of the configuration in a test-mode editor at the default window
// size (1040x700) and saves a PNG of every VP-V unit tab's Main and Advanced page, the Pipeline
// (expert) and Proposed retirement tabs, the Dynamic Drive page's Main and Advanced, and the
// Rendering page, in the VP-V and the Original pathway. Each picture is the visible window and,
// beside it ("-full"), the whole scrolled page at that width. Nothing is saved to any configuration.
int writeSettingsScreens(const QString& configPath, const QString& outDir)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("VideoProcessor.cfg"));
    if (!QFile::copy(configPath, path)) return 1;
    QDir().mkpath(outDir);
    ConfigEditorWindow window(path, 0, true);
    window.resize(1040, 700);
    window.show();
    QCoreApplication::processEvents();
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
    if (!pages) return 1;
    auto pageOf = [pages](QWidget* w) { for (int i = 0; i < pages->count(); ++i) if (pages->widget(i)->isAncestorOf(w)) return i; return -1; };
    auto scrollOf = [](QWidget* w) { QScrollArea* s = nullptr; for (QWidget* p = w; p && !s; p = p->parentWidget()) s = qobject_cast<QScrollArea*>(p); return s; };
    int count = 0;
    auto shoot = [&](const QString& name, QWidget* inPage)
    {
        QCoreApplication::processEvents();
        window.grab().save(QDir(outDir).filePath(name + QStringLiteral(".png")));
        if (QScrollArea* scroll = scrollOf(inPage))
            if (QWidget* content = scroll->widget()) content->grab().save(QDir(outDir).filePath(name + QStringLiteral("-full.png")));
        ++count;
    };
    auto* pathway = window.findChild<QComboBox*>(QStringLiteral("vpv.pathway"));
    auto* units = window.findChild<QTabWidget*>(QStringLiteral("vpv.units"));
    auto* drive = window.findChild<QTabWidget*>(QStringLiteral("vpv.drivepage.pages"));
    auto* rendering = window.findChild<QWidget*>(QStringLiteral("config.vprenderer.profiles"));
    if (!pathway || !units) return 1;
    for (int p = 0; p < 2; ++p)
    {
        pathway->setCurrentIndex(p);
        const QString tag = p == 0 ? QStringLiteral("vpv") : QStringLiteral("original");
        window.selectPage(pageOf(units));
        for (int u = 0; u < units->count(); ++u)
        {
            units->setCurrentIndex(u);
            const QString unit = units->tabText(u).toLower().replace(QLatin1Char(' '), QLatin1Char('-')).remove(QLatin1Char('(')).remove(QLatin1Char(')'));
            QTabWidget* sub = nullptr;
            for (QTabWidget* t : units->widget(u)->findChildren<QTabWidget*>())
                if (t->objectName().endsWith(QStringLiteral(".pages"))) sub = t;
            if (!sub) { shoot(QStringLiteral("%1-%2-%3").arg(tag).arg(u, 2, 10, QLatin1Char('0')).arg(unit), units); continue; }
            for (int s2 = 0; s2 < sub->count(); ++s2)
            {
                sub->setCurrentIndex(s2);
                shoot(QStringLiteral("%1-%2-%3-%4").arg(tag).arg(u, 2, 10, QLatin1Char('0')).arg(unit, sub->tabText(s2).toLower()), units);
            }
        }
        if (drive)
        {
            window.selectPage(pageOf(drive));
            for (int t = 0; t < drive->count(); ++t)
            {
                drive->setCurrentIndex(t);
                shoot(QStringLiteral("%1-dynamic-drive-%2").arg(tag, drive->tabText(t).toLower()), drive);
            }
        }
        if (rendering)
        {
            window.selectPage(pageOf(rendering));
            shoot(QStringLiteral("%1-rendering").arg(tag), rendering);
        }
    }
    window.hide();
    std::cout << "screens: " << count << " pictures in " << outDir.toStdString() << std::endl;
    return count > 0 ? 0 : 1;
}

int resaveConfiguration(const QString& path)
{
    ConfigEditorWindow window(path, 0, true);
    auto* apply = window.findChild<QPushButton*>(QStringLiteral("applyConfiguration"));
    if (!apply) return 1;
    if (!apply->isEnabled()) return 0;
    apply->click();
    QCoreApplication::processEvents();
    auto* status = window.findChild<QLabel*>(QStringLiteral("configurationStatus"));
    std::cout << "resave: " << (status ? status->text().toStdString() : std::string()) << std::endl;
    return apply->isEnabled() ? 1 : 0;
}

void testHunt2StartupControlsWriteParserSpellings()
{
    // Hunt 2 (2026-10-17, every setting has a control): DeckLink RGB packing, the DirectShow P010
    // conversion, the PPM correction and bounded_invalid_capture_recovery. Unset stays unset (no
    // write on open); each edit writes a value MainConfigSchema accepts.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\ncapture_device: BlackMagic DeckLink\n");
    const QByteArray original = readBytes(path);
    ConfigEditorWindow window(path, 0, true);
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    require(!apply->isEnabled(), "opening Config with the new controls unset marked the editor dirty");
    for (const char* name : { "config.decklink.rgb_8bit_packing", "config.decklink.rgb_10bit_packing",
        "config.decklink.rgb_12bit_packing", "config.directshow.conversion.conversion_method",
        "config.directshow.conversion.chroma_downsampling" })
        require(requireControl<QComboBox>(window, name)->currentData().toString().isEmpty(),
            (std::string(name) + " does not show Not set for an unset key").c_str());
    for (const char* name : { "config.directshow.conversion.min_core_count",
        "config.directshow.conversion.max_core_count", "config.directshow.ppm.ppm" })
        require(requireControl<QLineEdit>(window, name)->text().isEmpty(),
            (std::string(name) + " is not empty for an unset key").c_str());
    auto* recovery = requireControl<QCheckBox>(window, "config.general.bounded_invalid_capture_recovery");
    require(recovery->isChecked(), "bounded_invalid_capture_recovery does not show its runtime default (on)");
    require(readBytes(path) == original, "Config wrote the file without an edit");

    selectData(requireControl<QComboBox>(window, "config.decklink.rgb_8bit_packing"), "BGRA");
    selectData(requireControl<QComboBox>(window, "config.decklink.rgb_10bit_packing"), "R10L");
    selectData(requireControl<QComboBox>(window, "config.decklink.rgb_12bit_packing"), "R12L");
    selectData(requireControl<QComboBox>(window, "config.directshow.conversion.conversion_method"), "simd");
    selectData(requireControl<QComboBox>(window, "config.directshow.conversion.chroma_downsampling"), "ADVANCED");
    requireControl<QLineEdit>(window, "config.directshow.conversion.min_core_count")->setText("2");
    requireControl<QLineEdit>(window, "config.directshow.conversion.max_core_count")->setText("4");
    requireControl<QLineEdit>(window, "config.directshow.ppm.ppm")->setText("AUTO");
    recovery->setChecked(false);
    QCoreApplication::processEvents();
    require(apply->isEnabled(), "the edits did not enable Apply");
    apply->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    for (const char* expected : { "rgb_8bit_packing: BGRA", "rgb_10bit_packing: R10L", "rgb_12bit_packing: R12L",
        "conversion_method: simd", "chroma_downsampling: ADVANCED", "min_core_count: 2", "max_core_count: 4",
        "ppm: AUTO", "bounded_invalid_capture_recovery: false" })
        require(saved.contains(expected), (std::string("not saved: ") + expected + "\n" + saved.toStdString()).c_str());
    ConfigFile parsed;
    require(parsed.Load(path.toStdString()), "the saved file does not load");
    std::string error;
    require(MainConfigSchema::Validate(parsed, error), ("the parser rejects the saved values: " + error).c_str());
    ConfigEditorWindow reopened(path, 0, true);
    require(requireControl<QComboBox>(reopened, "config.decklink.rgb_10bit_packing")->currentData().toString() == "R10L",
        "the reopened Config does not show R10L");
    require(!requireControl<QCheckBox>(reopened, "config.general.bounded_invalid_capture_recovery")->isChecked(),
        "the reopened Config does not show bounded recovery off");
    // Not set removes the key again.
    selectData(requireControl<QComboBox>(reopened, "config.decklink.rgb_10bit_packing"), "");
    requireControl<QLineEdit>(reopened, "config.directshow.ppm.ppm")->clear();
    QCoreApplication::processEvents();
    requireControl<QPushButton>(reopened, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    const QByteArray cleared = readBytes(path);
    require(!cleared.contains("rgb_10bit_packing") && !cleared.contains("ppm: AUTO"),
        "Not set did not remove the keys");
}

void testHunt2SubtitleDetectionFields()
{
    // Hunt 2 (2026-10-17): subtitle_hold_ms / subtitle_near_bar_px are read from every Subtitles
    // profile (LVR) and had no control (the coverage allow-list called them legacy).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\nrenderer: VP Renderer\n"
        "[vprenderer.subtitles.Classic]\ntype: classic\n");
    ConfigEditorWindow window(path, 0, true);
    window.show(); QApplication::processEvents();
    auto* list = requireControl<QListWidget>(window, "config.vprenderer.subtitles.profiles");
    require(list->count() == 1, "the Subtitles profile is missing");
    list->setCurrentRow(0);
    QApplication::processEvents();
    auto* hold = requireControl<QLineEdit>(window, "config.vprenderer.subtitles.subtitle_hold_ms");
    auto* nearBar = requireControl<QLineEdit>(window, "config.vprenderer.subtitles.subtitle_near_bar_px");
    require(hold->text() == "250", ("hold does not show the runtime default 250: " + hold->text()).toStdString().c_str());
    require(nearBar->text() == "20", ("near-bar does not show the runtime default 20: " + nearBar->text()).toStdString().c_str());
    require(!requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(),
        "showing the Subtitles profile marked the editor dirty");
    hold->setText("400");
    nearBar->setText("35");
    QCoreApplication::processEvents();
    requireControl<QPushButton>(window, "applyConfiguration")->click();
    QCoreApplication::processEvents();
    const QByteArray saved = readBytes(path);
    require(saved.contains("subtitle_hold_ms: 400") && saved.contains("subtitle_near_bar_px: 35"),
        ("subtitle detection fields not saved: " + saved.toStdString()).c_str());
    ConfigFile parsed;
    require(parsed.Load(path.toStdString()), "the saved file does not load");
    std::string error;
    require(RendererProfileConfig::ValidateOwnedSections(parsed, error), ("the parser rejects the saved values: " + error).c_str());
}

void testAdvancedLedSettingsMatchTheParser()
{
    // Bug 8: every runtime LED key has a control whose default is the parser's.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[led_drive.Default]\nenabled: true\ncommand_source: compensate\n"
        "compensation_eotf: 2.415\ndeadband: 80\n[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
    const LedDrive::LedDriveConfig defaults;
    {
        ConfigEditorWindow window(path, 0, true);
        window.selectPage(20);
        auto number = [&](const char* key) {
            return requireControl<QDoubleSpinBox>(window, QStringLiteral("config.led_drive.default.") + key)->value();
        };
        require(number("tick_ms") == defaults.tickMs && number("ftb_black_ms") == defaults.ftbBlackMs &&
            number("led_ceiling") == defaults.ledCeiling && number("anti_pump_band") == defaults.antiPumpBand &&
            number("steps_up_split_apl") == defaults.stepsUpSplitApl && number("highlight_persist_ms") == defaults.highlightPersistMs &&
            number("apl_persist_ms") == defaults.aplPersistMs && number("highlight_release_ms") == defaults.highlightReleaseMs &&
            number("fade_ms") == defaults.fadeMs && number("script_highlight_code") == defaults.scriptHighlightCode &&
            std::abs(number("knee") - defaults.knee) < 1e-9 &&
            std::abs(number("highlight_percentile") - defaults.highlightPercentile) < 1e-9 &&
            std::abs(number("highlight_area_small") - defaults.highlightAreaSmall) < 1e-9 &&
            std::abs(number("highlight_area_large") - defaults.highlightAreaLarge) < 1e-9 &&
            std::abs(number("fade_min_slope") - defaults.fadeMinSlope) < 1e-9 &&
            std::abs(number("cut_threshold") - defaults.cutThreshold) < 1e-9,
            "An advanced LED default differs from the parser's");
        for (const char* key : { "early_ramp", "cut_jump", "bright_cut_dump" })
            require(requireControl<QCheckBox>(window, QStringLiteral("config.led_drive.default.") + key)->isChecked(),
                "An advanced LED switch does not show its default (on)");
        LedDrive::StepTable dark, bright, down;
        std::vector<LedDrive::CurvePoint> curve;
        double weights[3] = {};
        auto placeholder = [&](const char* key) {
            return requireControl<QLineEdit>(window, QStringLiteral("config.led_drive.default.") + key)->placeholderText().toStdString();
        };
        require(LedDrive::ParseStepTable(placeholder("steps_up_dark"), dark) && dark.rows == defaults.stepsUpDark.rows &&
            LedDrive::ParseStepTable(placeholder("steps_up_bright"), bright) && bright.rows == defaults.stepsUpBright.rows &&
            LedDrive::ParseStepTable(placeholder("steps_down"), down) && down.rows == defaults.stepsDown.rows &&
            LedDrive::ParseCurve(placeholder("curve"), curve) && curve.size() == defaults.curve.size() &&
            LedDrive::ParseWeights(placeholder("apl_weights"), weights) && weights[0] == defaults.aplWeights[0] &&
            weights[1] == defaults.aplWeights[1] && weights[2] == defaults.aplWeights[2],
            "An advanced LED table's shown default differs from the parser's");
        for (size_t i = 0; i < curve.size(); ++i)
            require(curve[i].apl == defaults.curve[i].apl && curve[i].level == defaults.curve[i].level,
                "The shown curve differs from the parser's");
        // (c): no rounding on load, out-of-range flagged.
        auto* gamma = requireControl<QDoubleSpinBox>(window, "config.led_drive.default.compensation_eotf");
        require(gamma->decimals() >= 3 && std::abs(gamma->value() - 2.415) < 1e-9, "The projector gamma was rounded");
        auto* deadband = requireControl<QDoubleSpinBox>(window, "config.led_drive.default.deadband");
        require(deadband->value() == 50 && deadband->property("clampedFrom").toString() == "80",
            "An out-of-range dead band is not flagged");
        auto* note = requireControl<QLabel>(window, "config.led_drive.default.clamped_note");
        require(note->text().contains("deadband = 80"), "The page does not note the clamped value");
        // Edits, a table, a reset and an invalid table.
        requireControl<QDoubleSpinBox>(window, "config.led_drive.default.tick_ms")->setValue(500);
        auto* steps = requireControl<QLineEdit>(window, "config.led_drive.default.steps_down");
        steps->setText("100:6, 50:3, 0:1");
        emit steps->textEdited(steps->text());
        requireControl<QToolButton>(window, "config.led_drive.default.deadband.reset")->click();
        require(deadband->value() == defaults.deadband, "Reset did not show the default");
        auto* weightsEdit = requireControl<QLineEdit>(window, "config.led_drive.default.apl_weights");
        weightsEdit->setText("0.5, 0.5, 0.5");
        emit weightsEdit->textEdited(weightsEdit->text());
        require(!requireControl<QLabel>(window, "config.led_drive.default.apl_weights.problem")->isHidden(),
            "An invalid weight list is not flagged");
        requireControl<QToolButton>(window, "config.led_drive.default.apl_weights.reset")->click();
        require(weightsEdit->text().isEmpty(), "The table reset did not clear the field");
        save(window);
    }
    ConfigFile config;
    require(config.Load(path.toStdString()), "The saved LED file did not load");
    std::string error;
    require(LedDrive::Validate(config, error), ("VP rejects the saved LED settings: " + error).c_str());
    const LedDrive::LedDriveConfig led = LedDrive::ReadLedDriveConfig(config);
    require(led.tickMs == 500 && led.deadband == defaults.deadband && led.stepsDown.rows.size() == 3 &&
        led.stepsDown.rows[0].second == 6, "VP does not read the advanced LED edits");
    const QByteArray saved = readBytes(path);
    require(!saved.contains("deadband") && !saved.contains("apl_weights") && saved.contains("compensation_eotf: 2.415"),
        (QByteArray("Reset did not remove the keys, or a value was rounded: ") + saved).constData());
}

void testDynamicDriveAliasSectionIsKept()
{
    // Bug hunt 2 2026-10-17 ("Dynamic Drive"): a file that spells the section
    // [dynamic_drive.Default] shows its values on the Dynamic Drive page and
    // keeps being saved there - no [led_drive] section is added.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[dynamic_drive.Default]\nenabled: true\ncommand_source: compensate\nled_floor: 90\n"
        "led_btb_passthrough: false\n[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
    {
        ConfigEditorWindow window(path, 0, true);
        window.selectPage(20);
        require(requireControl<QCheckBox>(window, "config.led_drive.default.enabled")->isChecked(),
            "The [dynamic_drive] enabled value was not read");
        require(requireControl<QComboBox>(window, "config.led_drive.default.command_source")->currentData().toString() ==
            "compensate", "The [dynamic_drive] command source was not read");
        require(requireControl<QSpinBox>(window, "config.led_drive.default.led_floor")->value() == 90,
            "The [dynamic_drive] LED floor was not read");
        auto* btb = requireControl<QCheckBox>(window, "config.led_drive.default.led_btb_passthrough");
        require(!btb->isChecked(), "The [dynamic_drive] led_btb_passthrough was not read");
        btb->setChecked(true);
        requireControl<QSpinBox>(window, "config.led_drive.default.led_floor")->setValue(100);
        save(window);
    }
    const QByteArray saved = readBytes(path);
    require(!saved.contains("[led_drive") && saved.count("[dynamic_drive.Default]") == 1 &&
        saved.contains("led_btb_passthrough: true") && saved.contains("led_floor: 100"),
        (QByteArray("The edits did not stay in [dynamic_drive.Default]: ") + saved).constData());
    ConfigFile config;
    require(config.Load(path.toStdString()), "The saved file did not load");
    std::string error;
    require(LedDrive::Validate(config, error), ("VP rejects the saved alias section: " + error).c_str());
    const LedDrive::LedDriveConfig led = LedDrive::ReadLedDriveConfig(config);
    require(led.enabled && led.btbPassthrough && led.ledFloor == 100, "VP does not read the saved alias section");

    // A fresh file keeps getting [led_drive.default].
    const QString fresh = dir.filePath("Fresh.cfg");
    writeFixture(fresh, "[projector.Default]\nprotocol: barco_ascii\nport: COM1\n");
    {
        ConfigEditorWindow window(fresh, 0, true);
        window.selectPage(20);
        requireControl<QCheckBox>(window, "config.led_drive.default.led_btb_passthrough")->setChecked(false);
        save(window);
    }
    const QByteArray freshSaved = readBytes(fresh);
    require(freshSaved.contains("led_btb_passthrough: false") && !freshSaved.contains("dynamic_drive") &&
        freshSaved.toLower().contains("[led_drive.default]"),
        (QByteArray("A fresh file did not get [led_drive.default]: ") + freshSaved).constData());
}

void testProfileBooleanCanBeUnset()
{
    // Bug 9: a named profile's own tick can be removed again ("Not set").
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\nquality: high\n[vprenderer.subtitles.Classic]\ntype: classic\n"
        "rounded_corners: false\n[vprenderer.subtitles.Black]\ntype: black\nrounded_corners: true\n");
    {
        ConfigEditorWindow window(path, 0, true);
        auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.subtitles.profiles");
        auto* rounded = requireControl<QCheckBox>(window, "config.vprenderer.subtitles.rounded_corners");
        auto* unset = requireControl<QToolButton>(window, "config.vprenderer.subtitles.rounded_corners.unset");
        profiles->setCurrentRow(0);
        QCoreApplication::processEvents();
        require(unset->isHidden(), "The default profile offers Not set");
        profiles->setCurrentRow(1);
        QCoreApplication::processEvents();
        require(rounded->isChecked() && !unset->isHidden(), "A named profile's own tick offers no Not set");
        unset->click();
        QCoreApplication::processEvents();
        require(!rounded->isChecked() && unset->isHidden(), "Not set does not show the default profile's value");
        save(window);
    }
    const QByteArray saved = readBytes(path);
    require(saved.count("rounded_corners") == 1 && saved.contains("rounded_corners: false"),
        (QByteArray("Not set did not remove the named profile's key: ") + saved).constData());
}

void testUnsavedEditsAskOnCloseAndExit()
{
    // Usability (a): closing or exiting with unsaved edits asks Save /
    // Discard / Keep editing.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    const QByteArray original = "[general]\nprofile_change_display_seconds: 5\n[vprenderer.Cinema]\nquality: high\n";
    writeFixture(path, original);
    ConfigEditorWindow window(path, 0, true);
    window.setPromptsForTesting(true);
    window.show();
    QCoreApplication::processEvents();
    requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds")->setValue(8);
    answerMessageBox(QMessageBox::Cancel);
    window.close();
    QCoreApplication::processEvents();
    require(window.isVisible() && requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(),
        "Keep editing closed Config or lost the edit");
    answerMessageBox(QMessageBox::Cancel);
    window.requestExit();
    QCoreApplication::processEvents();
    require(window.isVisible(), "Keep editing on Exit closed Config");
    answerMessageBox(QMessageBox::Save);
    window.close();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("profile_change_display_seconds: 8"), "Save on close did not save");
    window.show();
    QCoreApplication::processEvents();
    requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds")->setValue(4);
    answerMessageBox(QMessageBox::Discard);
    window.close();
    QCoreApplication::processEvents();
    require(readBytes(path).contains("profile_change_display_seconds: 8") && !window.isVisible(),
        "Discard on close saved or kept Config open");
}

void testExplicitAutoDitherShowsAuto()
{
    // Usability (b): one "Not set"; an explicit temporal_dither: auto is Auto.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\nquality: high\n[vprenderer.Other]\ntemporal_dither: auto\n");
    ConfigEditorWindow window(path, 0, true);
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(1);
    QCoreApplication::processEvents();
    auto* autoButton = requireControl<QRadioButton>(window, "config.vprenderer.temporal_dither.auto");
    auto* value = requireControl<QComboBox>(window, "config.vprenderer.temporal_dither");
    require(autoButton->isChecked() && value->currentData().toString() == "auto", "An explicit auto does not show Auto");
    int notSet = 0;
    for (int i = 0; i < value->count(); ++i)
        if (value->itemText(i).contains("Not set", Qt::CaseInsensitive)) ++notSet;
    require(notSet == 0 && value->itemData(0).toString().isEmpty(), "The dither still has two Not set items");
}

void testHiccupRepairIsSeparateFromMotionSmoothing()
{
    // Owner 2026-10-17: "Repair dropped frames / hiccups" is its own group; motion smoothing off
    // never disables it. Key names are unchanged.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\ninterpolation: off\n");
    ConfigEditorWindow window(path, 0, true);
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
    QCoreApplication::processEvents();
    auto* smoothing = requireControl<QLabel>(window, "config.vprenderer.interpolation_heading");
    auto* repair = requireControl<QLabel>(window, "config.vprenderer.hiccup_repair_heading");
    require(smoothing->text().contains("Motion smoothing (interpolation)") &&
        repair->text().contains("Repair dropped frames / hiccups"), "the two groups are not labelled");
    require(requireControl<QLabel>(window, "config.vprenderer.interpolation_note")->text().contains("Off for film"),
        "the motion smoothing note is missing");
    auto* fillGaps = requireControl<QComboBox>(window, "config.vprenderer.interpolation_fill_gaps");
    require(fillGaps->isEnabled() || fillGaps->property("vpvManaged").toBool(), "hiccup repair is disabled with interpolation off");
    // Owner decision 2026-10-18: hiccup repair is on by default; motion smoothing stays off.
    {
        const QString selected = fillGaps->currentData().toString();
        const QString effective = selected.isEmpty() ?
            fillGaps->property("inheritedValue").toString().trimmed().toLower() : selected;
        require(effective == QStringLiteral("on"),
            ("hiccup repair is not on by default: " + effective.toStdString()).c_str());
        require(fillGaps->toolTip().contains("On (default)"),
            ("the fill-gaps tooltip does not name On as the default: " + fillGaps->toolTip().toStdString()).c_str());
        require(requireControl<QRadioButton>(window, "config.vprenderer.interpolation.off")->isChecked(),
            "motion smoothing is not off");
    }
    requireControl<QRadioButton>(window, "config.vprenderer.interpolation.always")->click();
    QCoreApplication::processEvents();
    requireControl<QRadioButton>(window, "config.vprenderer.interpolation.off")->click();
    QCoreApplication::processEvents();
    require(fillGaps->isEnabled() || fillGaps->property("vpvManaged").toBool(), "switching motion smoothing off disabled hiccup repair");
    selectData(fillGaps, "on");
    save(window);
    const QByteArray saved = readBytes(path);
    require(saved.contains("interpolation_fill_gaps: on") && saved.contains("interpolation: off"),
        (QByteArray("the keys changed: ") + saved).constData());
    // Render-ahead 2026-10-17: the fill no longer needs motion smoothing; the wording says so.
    require(!fillGaps->toolTip().contains("Currently works only while motion smoothing") &&
        fillGaps->toolTip().contains("motion smoothing on or off"),
        ("the fill-gaps tooltip still says it needs motion smoothing: " + fillGaps->toolTip().toStdString()).c_str());
}

void testRenderAheadControlsShowDefaultsAndSave()
{
    // docs/VP-render-ahead-design.md: the four render-ahead keys have controls in Fixes under test,
    // unset shows the renderer's defaults, and each edit saves under its key.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\nquality: high\n");
    ConfigEditorWindow window(path, 0, true);
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
    QCoreApplication::processEvents();
    auto* renderAhead = requireControl<QComboBox>(window, "config.vprenderer.render_ahead");
    auto* gate = requireControl<QComboBox>(window, "config.vprenderer.dejudder_gate");
    auto* utilisation = requireControl<QLineEdit>(window, "config.vprenderer.render_ahead_utilisation");
    auto* credit = requireControl<QComboBox>(window, "config.vprenderer.render_ahead_queue_credit");
    require(renderAhead->currentIndex() == 0 && renderAhead->itemText(0).contains("off", Qt::CaseInsensitive),
        ("An unset render ahead does not say its default (off): " + renderAhead->itemText(0).toStdString()).c_str());
    // On the default profile an unset choice whose default is Auto shows the Auto choice itself
    // (the "Default" row is hidden, as for the other auto defaults).
    require(gate->currentText().contains("auto", Qt::CaseInsensitive) &&
        (gate->currentIndex() == 0 || gate->currentData().toString() == "auto"),
        ("An unset de-judder gate does not say its default (auto): " + gate->currentText().toStdString()).c_str());
    require(credit->currentIndex() == 0 && credit->itemText(0).contains("off", Qt::CaseInsensitive),
        ("An unset queue credit does not say its default (off): " + credit->itemText(0).toStdString()).c_str());
    require(utilisation->text().isEmpty() && utilisation->placeholderText() == "0.85",
        ("An unset utilisation does not show its default 0.85: '" + utilisation->text().toStdString() + "' / '" +
            utilisation->placeholderText().toStdString() + "'").c_str());
    require(utilisation->validator() != nullptr, "the utilisation field has no validator");
    {
        QString tooHigh = QStringLiteral("1.5"); int pos = 0;
        require(utilisation->validator()->validate(tooHigh, pos) != QValidator::Acceptable,
            "the utilisation field accepts 1.5");
        QString inRange = QStringLiteral("0.9"); pos = 0;
        require(utilisation->validator()->validate(inRange, pos) == QValidator::Acceptable,
            "the utilisation field rejects 0.9");
    }
    require(!renderAhead->toolTip().isEmpty() && !gate->toolTip().isEmpty() && !utilisation->toolTip().isEmpty() &&
        !credit->toolTip().isEmpty(), "a render-ahead control has no tooltip");
    selectData(renderAhead, "on");
    selectData(gate, "period");
    utilisation->setText("0.9");
    selectData(credit, "on");
    save(window);
    const QByteArray saved = readBytes(path);
    require(saved.contains("render_ahead: on") && saved.contains("dejudder_gate: period") &&
        saved.contains("render_ahead_utilisation: 0.9") && saved.contains("render_ahead_queue_credit: on") &&
        saved.contains("quality: high"),
        (QByteArray("the render-ahead keys did not save: ") + saved).constData());
}

void testUnfocusedNumberBoxesIgnoreTheWheel()
{
    // Usability (d): the wheel scrolls the page, not an unfocused setting.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\nprofile_change_display_seconds: 5\n[vprenderer.Cinema]\nquality: high\n");
    ConfigEditorWindow window(path, 0, true);
    auto* seconds = requireControl<QSpinBox>(window, "config.general.profile_change_display_seconds");
    auto* refresh = requireControl<QComboBox>(window, "config.general.switch_refresh_rate");
    const int comboIndex = refresh->currentIndex();
    for (QWidget* control : { static_cast<QWidget*>(seconds), static_cast<QWidget*>(refresh) })
    {
        QWheelEvent wheel(QPointF(5, 5), control->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, 120),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(control, &wheel);
    }
    require(seconds->value() == 5 && refresh->currentIndex() == comboIndex, "The wheel changed an unfocused setting");
    require(!requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(), "The wheel made an edit");
    // Hunt 2 (M3): scrolling the page past the Motion or Picture slider changes nothing.
    for (const char* name : { "config.vprenderer.motion_preset", "config.vprenderer.picture_preset" })
    {
        auto* slider = requireControl<QSlider>(window, name);
        const int before = slider->value();
        for (const int delta : { 120, -120 })
        {
            QWheelEvent wheel(QPointF(5, 5), slider->mapToGlobal(QPointF(5, 5)), QPoint(), QPoint(0, delta),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(slider, &wheel);
            require(slider->value() == before, (QByteArray("The wheel moved the unfocused slider ") + name).constData());
        }
    }
    require(!requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(), "The wheel over a slider made an edit");
}

// Hunt 2 (L4/L5/L6): an inherited Picture level is "Not set" and leaving an inherited
// field writes nothing; render ahead's settings are greyed while it is off; an
// out-of-range share is clamped when the field is left.
void testInheritedPictureFieldsAndGreyedRenderAhead()
{
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\nprofile_change_display_seconds: 5\n"
        "[vprenderer.Cinema]\nquality: high\nsource_resolution_auto_min_confidence: 0.70\n"
        "source_resolution_evidence_seconds: 5\nsource_resolution_evidence_shots: 2\n"
        "[vprenderer.Night]\nquality: high\n");
    ConfigEditorWindow window(path, 0, true);
    auto* apply = requireControl<QPushButton>(window, "applyConfiguration");
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(1);
    QCoreApplication::processEvents();
    require(requireControl<QLabel>(window, "config.vprenderer.picture_preset_name")->text().startsWith("Not set"),
        "An inherited Picture level showed as this profile's own");
    for (const char* name : { "config.vprenderer.source_resolution_auto_min_confidence",
        "config.vprenderer.source_resolution_evidence_seconds", "config.vprenderer.source_resolution_evidence_shots" })
        QMetaObject::invokeMethod(requireControl<QLineEdit>(window, name), "editingFinished", Qt::DirectConnection);
    QCoreApplication::processEvents();
    require(!apply->isEnabled(), "Leaving an inherited Picture field wrote an override");
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
    QCoreApplication::processEvents();
    auto* share = requireControl<QLineEdit>(window, "config.vprenderer.render_ahead_utilisation");
    require(!share->isEnabled() && !requireControl<QComboBox>(window, "config.vprenderer.dejudder_gate")->isEnabled() &&
        !requireControl<QComboBox>(window, "config.vprenderer.render_ahead_queue_credit")->isEnabled(),
        "Render ahead's settings are not greyed while it is off");
    selectData(requireControl<QComboBox>(window, "config.vprenderer.render_ahead"), "on");
    require(share->isEnabled(), "Render ahead on did not enable its share");
    share->setText("0.3");
    QMetaObject::invokeMethod(share, "editingFinished", Qt::DirectConnection);
    require(share->text() == "0.5", ("An out-of-range share was not clamped: " + share->text()).toStdString().c_str());
}

// Hunt 2 (M1/M2): the legacy queue fold and the profile value rewrites run once at
// load as real migrations. The editor shows them as unsaved changes, the file is not
// written until Save, Save keeps the original in .before-config-migration.<time>.bak,
// and afterwards displaying every profile changes nothing.
void testProfileRewritesAreBackedUpLoadMigrations()
{
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    const QByteArray original =
        "[general]\nprofile_change_display_seconds: 5\nrenderer: VideoProcessor Renderer (Alpha)\n"
        "[directshow]\npresentation_lead_frames: 2\n"
        "[queue.Steady]\nsteady_reserve_frames: 3\n"
        "[vprenderer.Cinema]\nquality: high\n"
        "[vprenderer.scaling.Sharp]\ndownscaler: ewa_lanczos\n"
        "[vprenderer.zoom.Scope]\ncrop_wider_content_to_fill_screen: true\n"
        "crop_wider_content_aspect_limit: not-a-ratio\n";
    writeFixture(path, original);
    const auto showEveryProfile = [](ConfigEditorWindow& window)
    {
        for (const char* name : { "config.queue.profiles", "config.vprenderer.profiles",
            "config.vprenderer.scaling.profiles", "config.vprenderer.zoom.profiles",
            "config.vprenderer.color.profiles" })
        {
            auto* profiles = requireControl<QListWidget>(window, name);
            for (int row = profiles->count() - 1; row >= 0; --row)
                profiles->setCurrentRow(row);
        }
        QCoreApplication::processEvents();
    };
    {
        ConfigEditorWindow window(path, 0, true);
        require(requireControl<QPushButton>(window, "applyConfiguration")->isEnabled(),
            "A load-time migration did not show as an unsaved change");
        showEveryProfile(window);
        require(readBytes(path) == original, "Opening and displaying profiles wrote the file");
        require(QDir(dir.path()).entryList({ QStringLiteral("*.bak") }, QDir::Files).isEmpty(),
            "A backup was made without a save");
        save(window);
    }
    const QByteArray saved = readBytes(path);
    require(!saved.contains("presentation_lead_frames") && !saved.contains("steady_reserve_frames") &&
        saved.contains("lead_frames: 2") && saved.contains("target_frames: 3"),
        (QByteArray("The legacy queue fold was not saved: ") + saved).constData());
    require(saved.contains("downscaler: AUTO") && !saved.contains("ewa_lanczos") &&
        !saved.contains("not-a-ratio") && saved.contains("crop_wider_content_to_fill_screen: true"),
        (QByteArray("The profile value migrations were not saved: ") + saved).constData());
    require(saved.contains("renderer: VP Renderer") && !saved.contains("(Alpha)"),
        (QByteArray("The former renderer name was not migrated: ") + saved).constData());
    const QStringList backups = QDir(dir.path()).entryList(
        { QStringLiteral("VideoProcessor.cfg.before-config-migration.*.bak") }, QDir::Files);
    require(backups.size() == 1 && readBytes(dir.filePath(backups.front())) == original,
        "The migration did not keep an exact backup of the original file");
    {
        ConfigEditorWindow reopened(path, 0, true);
        require(!requireControl<QPushButton>(reopened, "applyConfiguration")->isEnabled(),
            "A migrated file opens with unsaved changes");
        showEveryProfile(reopened);
        require(!requireControl<QPushButton>(reopened, "applyConfiguration")->isEnabled(),
            "Displaying a profile changed the document");
    }
    require(readBytes(path) == saved, "Reopening the migrated file wrote it");
}

void testFilePlaybackControlsValidateResetAndSave()
{
    // VP Renderer filter (2026-10-17): [general] file_av_offset_ms and file_led_holdover_ms have
    // number boxes with the schema's ranges, show the defaults when unset, reset to the default
    // and save under their keys.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[general]\nfile_av_offset_ms: -120\n[vprenderer.Cinema]\nquality: high\n");
    ConfigEditorWindow window(path, 0, true);
    auto* offset = requireControl<QSpinBox>(window, "config.general.file_av_offset_ms");
    auto* holdover = requireControl<QSpinBox>(window, "config.general.file_led_holdover_ms");
    require(offset->value() == -120, "the configured picture offset is not shown");
    require(holdover->value() == MainConfigSchema::FileLedHoldoverDefaultMs,
        "an unset LED hold-over does not show its default");
    require(offset->minimum() == MainConfigSchema::FileAvOffsetMinMs &&
        offset->maximum() == MainConfigSchema::FileAvOffsetMaxMs, "the picture offset range is not the schema's");
    require(holdover->minimum() == MainConfigSchema::FileLedHoldoverMinMs &&
        holdover->maximum() == MainConfigSchema::FileLedHoldoverMaxMs, "the LED hold-over range is not the schema's");
    require(!offset->toolTip().isEmpty() && !holdover->toolTip().isEmpty(), "a file playback control has no tooltip");
    require(readBytes(path).indexOf("file_led_holdover_ms") < 0, "opening Config wrote the unset hold-over");
    offset->setValue(5000);
    require(offset->value() == MainConfigSchema::FileAvOffsetMaxMs, "the picture offset accepted a value out of range");
    holdover->setValue(1500);
    requireControl<QToolButton>(window, "config.general.file_av_offset_ms.reset")->click();
    require(offset->value() == MainConfigSchema::FileAvOffsetDefaultMs, "reset did not restore the default offset");
    save(window);
    const QByteArray saved = readBytes(path);
    require(saved.contains("file_av_offset_ms: 0") && saved.contains("file_led_holdover_ms: 1500") &&
        saved.contains("quality: high"), (QByteArray("the file playback keys did not save: ") + saved).constData());
}

// Bug hunt 3 (2026-10-18, hunter D): Config findings CFG2-CFG7.
void testHunt3LatchTimingFieldsShowDefaultsAndGrey()
{
    // CFG2: the latch timing fields show 20 / 1.5 / 0.25 (not "Auto") and are greyed
    // unless Source resolution is Auto with the latch fixes on.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\nquality: high\n");
    ConfigEditorWindow window(path, 0, true);
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
    QCoreApplication::processEvents();
    const std::pair<const char*, const char*> fields[] = { { "source_resolution_engage_max_wait_s", "20" },
        { "source_resolution_crossfade_s", "1.5" }, { "source_resolution_release_fade_s", "0.25" } };
    for (const auto& field : fields)
    {
        auto* edit = requireControl<QLineEdit>(window, QStringLiteral("config.vprenderer.") + field.first);
        require(edit->text().isEmpty() && edit->placeholderText() == QLatin1String(field.second),
            (std::string(field.first) + " shows '" + edit->text().toStdString() + "' / placeholder '" +
                edit->placeholderText().toStdString() + "', not its default " + field.second).c_str());
        require(edit->isEnabled(), (std::string(field.first) + " is greyed with Auto and the latch fixes on").c_str());
    }
    selectData(requireControl<QComboBox>(window, "config.vprenderer.source_resolution_latch_fixes"), "off");
    for (const auto& field : fields)
        require(!requireControl<QLineEdit>(window, QStringLiteral("config.vprenderer.") + field.first)->isEnabled(),
            (std::string(field.first) + " is not greyed with the latch fixes off").c_str());
    selectData(requireControl<QComboBox>(window, "config.vprenderer.source_resolution_latch_fixes"), "on");
    selectData(requireControl<QComboBox>(window, "config.vprenderer.source_resolution"), "native");
    for (const auto& field : fields)
        require(!requireControl<QLineEdit>(window, QStringLiteral("config.vprenderer.") + field.first)->isEnabled(),
            (std::string(field.first) + " is not greyed with Source resolution: Native").c_str());
}

void testHunt3RetiredEvenRatesSayWhatTheyMapTo()
{
    // CFG3: a retired dejudder_even_rates value says what it maps to, and the Motion
    // slider reads it as that value (the Cinema level, not Custom).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\nquality: high\ndejudder_strength: 0.25\ndejudder_strength_even: 0\n"
        "dejudder_even_rates: 30\ndejudder_object_strength: 0.5\ndejudder_motion_rolloff: on\n"
        "dejudder_roll_floor: 0.25\ndejudder_roll_lo: 8\ndejudder_roll_hi: 24\n");
    ConfigEditorWindow window(path, 0, true);
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
    QCoreApplication::processEvents();
    auto* evenRates = requireControl<QComboBox>(window, "config.vprenderer.dejudder_even_rates");
    require(evenRates->currentData().toString() == "30" && evenRates->currentText().contains("retired") &&
        evenRates->currentText().contains("Whole-number"),
        ("the retired value does not say what it maps to: " + evenRates->currentText().toStdString()).c_str());
    auto* name = requireControl<QLabel>(window, "config.vprenderer.motion_preset_name");
    require(name->text().startsWith("Cinema"),
        ("the Motion slider does not read the retired value as whole: " + name->text().toStdString()).c_str());
}

void testHunt3InterpolationGrainFollowsHiccupRepair()
{
    // CFG4: the grain choice is used by hiccup repair's gap fill, so it is not greyed
    // while Fill dropped frames is on (motion smoothing off).
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\nquality: high\ninterpolation: off\ninterpolation_fill_gaps: on\n");
    ConfigEditorWindow window(path, 0, true);
    requireControl<QListWidget>(window, "config.vprenderer.profiles")->setCurrentRow(0);
    QCoreApplication::processEvents();
    auto* grain = requireControl<QComboBox>(window, "config.vprenderer.interpolation_grain");
    require(grain->isEnabled(), "interpolation_grain is greyed while hiccup repair uses it");
    selectData(requireControl<QComboBox>(window, "config.vprenderer.interpolation_fill_gaps"), "off");
    require(!grain->isEnabled(), "interpolation_grain is not greyed with interpolation and hiccup repair off");
}

void testHunt3ContentGatedProfileGreysClassifierInputs()
{
    // CFG5: in a profile whose when: reads $content, the renderer ignores the classifier's
    // own inputs (with cadence_class_fixes on): greyed, with the reason; other profiles are not.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[vprenderer.Cinema]\nquality: high\n"
        "[vprenderer.Sports]\nwhen: $content == \"sports\"\nsource_class: video\n");
    ConfigEditorWindow window(path, 0, true);
    auto* profiles = requireControl<QListWidget>(window, "config.vprenderer.profiles");
    profiles->setCurrentRow(1);
    QCoreApplication::processEvents();
    for (const char* key : { "intent_content.check", "source_class", "dejudder_pulldown", "cadence_class_fixes" })
    {
        auto* widget = requireControl<QWidget>(window, QStringLiteral("config.vprenderer.") + key);
        require(!widget->isEnabled() && widget->toolTip().contains("Ignored in this profile"),
            (std::string(key) + " is not greyed with the reason in a content-gated profile").c_str());
    }
    profiles->setCurrentRow(0);
    // Organisation pass 2: the proven fixes (cadence_class_fixes among them) are set by their master
    // (Proposed retirement tab) until Custom.
    requireControl<QCheckBox>(window, "vpv.retirement.card.fixes.custom")->setChecked(true);
    QCoreApplication::processEvents();
    for (const char* key : { "intent_content.check", "dejudder_pulldown", "cadence_class_fixes" })
    {
        auto* widget = requireControl<QWidget>(window, QStringLiteral("config.vprenderer.") + key);
        require(widget->isEnabled() && !widget->toolTip().contains("Ignored in this profile"),
            (std::string(key) + " stayed greyed in a profile without a content rule").c_str());
    }
}

void testHunt3LegacyQueueFoldTargetsQueueSection()
{
    // CFG6: legacy queue values fold into [queue] (what VP reads first), not the first
    // queue section in the file, and [queue] (renamed, as Config saves only [queue.*])
    // stays VP's default; CFG9: [general] queue_size folds too.
    QTemporaryDir dir; const QString path = dir.filePath("VideoProcessor.cfg");
    writeFixture(path, "[queue.Game]\nwhen: ${source_rate} > 59\nqueue_size: 2\n"
        "[queue]\ntarget_frames: 3\n"
        "[directshow]\npresentation_lead_frames: 4\n"
        "[general]\nfullscreen: false\nqueue_size: 24\n"
        "[vprenderer.Cinema]\nquality: high\n");
    ConfigEditorWindow window(path, 0, true);
    QCoreApplication::processEvents();
    save(window);
    ConfigFile saved;
    require(saved.Load(path.toStdString()), "the saved file does not load");
    std::string text = readBytes(path).toStdString();
    std::replace(text.begin(), text.end(), '\n', '|');
    std::string value, defaultQueue;
    require(QueueConfiguration::ResolveDefaultSection(saved, defaultQueue) && defaultQueue != "queue.Game",
        ("the former [queue] is no longer VP's default queue: " + text).c_str());
    require(saved.TryGetString(defaultQueue, "target_frames", value) && value == "3",
        ("the default queue is not the former [queue]: " + text).c_str());
    require(saved.TryGetString(defaultQueue, "lead_frames", value) && value == "4",
        ("presentation_lead_frames did not fold into [queue]: " + text).c_str());
    require(saved.TryGetString(defaultQueue, "queue_size", value) && value == "24",
        ("[general] queue_size did not fold into [queue]: " + text).c_str());
    require(saved.TryGetString("queue.Game", "queue_size", value) && value == "2",
        "the first queue section was changed by the fold");
    require(!saved.TryGetString("queue.Game", "lead_frames", value), "the fold wrote into the first queue section");
    require(!saved.TryGetString("general", "queue_size", value), "[general] queue_size stayed after the fold");
}

void testHunt3RemoteDiscardFoldsLikeALoad()
{
    // CFG7: Discard on a remote target runs the same input-policy fold and empty-header
    // removal as a fresh load (the controls show the folded value after Cancel).
    using namespace ConfigurationRpcProtocol;
    QTemporaryDir directory;
    const QString path = directory.filePath("VideoProcessor.cfg");
    const std::string remoteBytes =
        "[general]\nfullscreen: false\n[vprenderer.input]\nvideo_conversion: V210_TO_P010\n"
        "[vprenderer.Cinema]\nquality: high\n";
    writeFixture(path, QByteArray::fromStdString(remoteBytes));
    WSADATA data{};
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Cannot initialize the RPC fixture");
    struct WinsockCleanup { ~WinsockCleanup() { WSACleanup(); } } cleanup;
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "Cannot open the RPC listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        listen(listener, 4) == 0, "Cannot listen for RPC requests");
    int addressSize = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0,
        "Cannot read the RPC port");
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::thread server([listener, path = path.toStdString(), remoteBytes, finished]
    {
        const auto receiveExact = [](SOCKET socket, uint8_t* bytes, size_t count)
        {
            while (count != 0)
            {
                const int received = recv(socket, reinterpret_cast<char*>(bytes), static_cast<int>(count), 0);
                if (received <= 0) return false;
                bytes += received;
                count -= received;
            }
            return true;
        };
        while (!finished->load())
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener, &readable);
            timeval timeout{ 0, 50000 };
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
            const SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) break;
            std::array<uint8_t, HeaderBytes> header{};
            uint8_t compatibility[2]{};
            if (receiveExact(client, header.data(), header.size()) &&
                receiveExact(client, compatibility, sizeof(compatibility)))
            {
                const uint16_t operation = Read16(header.data() + 6);
                Frame response;
                response.operation = operation | ResponseFlag;
                if (operation == static_cast<uint16_t>(Operation::GetCapabilities))
                {
                    Write16(response.payload, ConfigurationCompatibilityVersion);
                    WriteString(response.payload, "1.3.test");
                    for (int list = 0; list < 6; ++list)
                        Write32(response.payload, 0);
                }
                else if (operation == static_cast<uint16_t>(Operation::GetConfig))
                {
                    WriteString(response.payload, path);
                    WriteString(response.payload, remoteBytes);
                }
                std::vector<uint8_t> wire;
                Encode(response, wire);
                size_t sent = 0;
                while (sent < wire.size())
                {
                    const int count = send(client, reinterpret_cast<const char*>(wire.data() + sent),
                        static_cast<int>(wire.size() - sent), 0);
                    if (count <= 0) break;
                    sent += count;
                }
            }
            closesocket(client);
        }
        closesocket(listener);
    });
    struct Join
    {
        std::thread& thread; std::shared_ptr<std::atomic<bool>> finished;
        ~Join() { finished->store(true); if (thread.joinable()) thread.join(); }
    } join{ server, finished };
    {
        ConfigEditorWindow window(path, 0, true, {}, {}, QStringLiteral("localhost"),
            ntohs(address.sin_port), QStringLiteral("TEST VP"));
        QCoreApplication::processEvents();
        const auto conversion = [&window]
        {
            return requireControl<QComboBox>(window,
                QStringLiteral("config.vprenderer.input_processing.video_conversion"))->currentData().toString();
        };
        require(conversion() == "V210_TO_P010", ("the remote load did not fold the input policy: " +
            conversion().toStdString()).c_str());
        requireControl<QPushButton>(window, "cancelConfiguration")->click();
        QCoreApplication::processEvents();
        require(conversion() == "V210_TO_P010", ("the remote Discard did not fold the input policy like a load: " +
            conversion().toStdString()).c_str());
    }
}

int run(const char* name, const std::function<void()>& test)
{
    const QString testName = QString::fromUtf8(name);
    if (!testNameFilter.isEmpty() &&
        testNameFilter.compare(testName, Qt::CaseInsensitive) != 0)
        return 0;
    if (!testNameMatches.isEmpty())
    {
        bool matched = false;
        for (const QString& word : testNameMatches)
            matched = matched || testName.contains(word, Qt::CaseInsensitive);
        if (!matched) return 0;
    }
    // Background runs (owner 2026-10-16: the tests made the computer unusable): with
    // QT_QPA_PLATFORM=offscreen no window is shown and focus is never taken; the few tests that need a
    // real native window (a Windows message to the editor's handle) are skipped and listed - run them
    // on screen (tools/run_config_tests.ps1 -OnScreen) when the computer is free.
    if (qEnvironmentVariable("QT_QPA_PLATFORM").compare(QStringLiteral("offscreen"), Qt::CaseInsensitive) == 0)
    {
        static const char* const nativeWindowTests[] = { "update shutdown discards edits without prompt" };
        for (const char* native : nativeWindowTests)
            if (testName.compare(QString::fromLatin1(native), Qt::CaseInsensitive) == 0)
            {
                if (testShardCount <= 1 || testShard == 0)   // reported once, not by every shard
                    std::cerr << "SKIP " << name << ": needs a real window (run on screen)" << std::endl;
                return 0;
            }
    }
    if (testShardCount > 1)
    {
        static const char* const serialWords[] = { "window", "focus", "z-order", "zorder", "foreground",
            "presentation target", "lease", "screen", "monitor", "move" };
        bool serial = false;
        for (const char* word : serialWords)
            serial = serial || testName.contains(QString::fromLatin1(word), Qt::CaseInsensitive);
        const int sequence = testSequence++;
        if (serial ? testShard != 0 : sequence % testShardCount != testShard)
            return 0;
    }
    ++selectedTestsRun;
    std::cerr << "START " << name << std::endl;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsedMs = [&started]()
    {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    };
    try
    {
        test();
        std::cout << "PASS " << name << " [" << elapsedMs() << " ms]" << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << name << ": " << error.what() << " [" << elapsedMs() << " ms]" << std::endl;
        return 1;
    }
}
}

int main(int argc, char** argv)
{
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) ==
        QStringLiteral("--zorder-helper"))
    {
        bool ok = false;
        const quintptr receiver = QString::fromLocal8Bit(argv[2]).toULongLong(&ok);
        return ok ? runExternalZOrderFixture(reinterpret_cast<HWND>(receiver)) : 2;
    }
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const QString option = QString::fromLocal8Bit(argv[i]);
        const QString value = QString::fromLocal8Bit(argv[i + 1]);
        if (option == QStringLiteral("--test"))
            testNameFilter = value;
        else if (option == QStringLiteral("--match"))
            testNameMatches = value.split(QLatin1Char('|'), Qt::SkipEmptyParts);
        else if (option == QStringLiteral("--shard"))
        {
            const QStringList parts = value.split(QLatin1Char('/'));
            if (parts.size() == 2 && parts[1].toInt() > 0)
            {
                testShard = parts[0].toInt();
                testShardCount = parts[1].toInt();
            }
        }
    }
    QApplication application(argc, argv);
    // Not tests: the settings coverage inventory (--inventory <file>) and the preview's re-save (--resave <file>).
    for (int i = 1; i + 1 < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--inventory"))
        {
            QApplication::setStyle(VpTheme::CreateStyle());
            application.setStyleSheet(VpTheme::StyleSheet());
            return writeSettingsInventory(QString::fromLocal8Bit(argv[i + 1]));
        }
        else if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--screens") && i + 2 < argc)
        {
            QApplication::setStyle(VpTheme::CreateStyle());
            application.setStyleSheet(VpTheme::StyleSheet());
            return writeSettingsScreens(QString::fromLocal8Bit(argv[i + 1]), QString::fromLocal8Bit(argv[i + 2]));
        }
        else if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--resave"))
        {
            QApplication::setStyle(VpTheme::CreateStyle());
            application.setStyleSheet(VpTheme::StyleSheet());
            return resaveConfiguration(QString::fromLocal8Bit(argv[i + 1]));
        }
    if (!qEnvironmentVariable("VP_CONFIG_REVIEW_IMAGES").isEmpty())
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
    QApplication::setStyle(VpTheme::CreateStyle());
    application.setStyleSheet(VpTheme::StyleSheet());
    int failures = 0;
    failures += run("Motion and chroma controls persist and expose new tab",testMotionAndChromaControlsPersistAndExposeNewTab);
    failures += run("integrated updates respect local package", testIntegratedUpdatesRespectLocalPackage);
    failures += run("update shutdown discards edits without prompt", testUpdateShutdownDiscardsEditsWithoutPrompt);
    failures += run("Tone slider drag performance", testToneSliderDragPerformance);
    failures += run("Tone mapping controls round trip", testToneMappingControls);
    failures += run("Beta tone mapping controls", testBetaToneMappingControls);
    failures += run("subtitle profile tab round trip", testSubtitleProfileTabRoundTrip);
    failures += run("subtitle background shape round trip and visibility", testSubtitleBackgroundShapeRoundTripAndVisibility);
    failures += run("subtitle text size reduction round trip and style visibility", testSubtitleTextSizeReductionRoundTripAndStyleVisibility);
    failures += run("subtitle HDR legacy fallback and shared modes", testSubtitleHdrLegacyFallbackAndSharedModes);
    failures += run("subtitle diagnostic survives Config save and reopen",
        testSubtitleDiagnosticSurvivesConfigSaveAndReopen);
    failures += run("Subtitle cut paste survives config save and reopen", testSubtitleCutPasteSurvivesConfigSaveAndReopen);
    failures += run("retired renderer settings survive config save and reopen", testRetiredRendererSettingsSurviveConfigSaveAndReopen);
    failures += run("audited Boolean aliases and inherited Auto status", testAuditedBooleanAliasesAndInheritedAutoStatus);
    failures += run("invalid audited values remain actionable", testInvalidAuditedValuesRemainActionable);
    failures += run("diagnostic preset describes flags and retention default", testDiagnosticPresetDescribesFlagsAndRetentionUsesDefault);
    failures += run("omitted and Auto frame offset display ninety", testOmittedAndAutoFrameOffsetDisplayNinety);
    failures += run("unified Color Output migration and transfer round trip", testUnifiedColorOutputMigrationAndTransferRoundTrip);
    failures += run("calibrated UI defaults and derived transport", testCalibratedUiDefaultsAndDerivedTransport);
    failures += run("calibrated legacy and inherited output preserved", testCalibratedLegacyAndInheritedOutputPreserved);
    failures += run("shared profile list controller contract",
        testSharedProfileListControllerContract);
    failures += run("Limited transport gamma notice and correction", testLimitedTransportGammaNoticeAndCorrection);
    failures += run("black default and legacy Auto inheritance", testBlackDefaultAndLegacyAutoInheritance);
    failures += run("Inherited HDR target luminance validation", testInheritedHdrTargetLuminanceValidation);
    failures += run("HDR target luminance validation retains saved value",
        testHdrTargetLuminanceValidationRetainsSavedValue);
    failures += run("every page round trips", testEveryPageRoundTrips);
    failures += run("renderer section tabs stay synchronized during rapid clicks",
        testRendererSectionTabsRemainSynchronizedDuringRapidClicks);
    failures += run("empty Standard shaders can create first shader",
        testEmptyStandardShadersCanCreateFirstShader);
    failures += run("shader profile lists can return to empty without synthetic roots",
        testShaderProfileListsCanReturnToEmptyWithoutSyntheticRoots);
    failures += run("two-column cards share row height", testTwoColumnCardsShareRowHeight);
    failures += run("inherited renderer Input selectors use effective labels",
        testInheritedRendererInputSelectorsUseEffectiveLabels);
    failures += run("inherited renderer Input selectors refresh with General",
        testInheritedRendererInputSelectorsRefreshWithGeneral);
    failures += run("empty shortcuts survive NLS backend edit",
        testEmptyShortcutsSurviveNlsBackendEdit);
    failures += run("SDR gamma adjustment labels and persistence",
        testSdrGammaAdjustmentLabelsAndPersistence);
    failures += run("calibration layout preserves independent slots", testCalibrationLayoutKeepsIndependentLutSlots);
    failures += run("calibration controls follow confirmed runtime LUT", testCalibrationControlsFollowConfirmedRuntimeLut);
    failures += run("HDR target gamma loads prior input aliases", testHdrTargetGammaLoadsPriorInputAliases);
    failures += run("independent SDR LUT gamma", testIndependentSdrLutGamma);
    failures += run("calibration profile owns LUT and HDR gamma", testCalibrationProfileOwnsLutAndHdrGamma);
    failures += run("legacy Rendering color settings migrate to Color Config",
        testLegacyRendererColorSettingsMigrateToColorConfigs);
    failures += run("separated profiles do not leave shortcut shells behind",
        testSeparatedProfilesDoNotLeaveShortcutShellsBehind);
    failures += run("legacy VP input override migrates independently",
        testLegacyVpInputOverrideMigratesToIndependentPolicySection);
    failures += run("LLDV named metadata is preserved",
        testLldvNamedMetadataIsPreserved);
    failures += run("optional config sections are created when edited",
        testOptionalSectionsAreCreatedWhenEdited);
    failures += run("DV mode selector preserves LLDV settings", testDvModeSelectorPreservesLldvSettings);
    failures += run("LLDV standard profiles", testLldvStandardProfiles);
    failures += run("profile lifecycle through widgets", testProfileLifecycleThroughWidgets);
    failures += run("unrelated content remains exact", testUnrelatedContentRemainsExact);
    failures += run("scene detection defaults off and hides manual overrides",
        testSceneDetectionDefaultsOffAndHidesManualOverrides);
    failures += run("virtual shader Off option persists", testVirtualShaderOffOptionPersistsWhenConfigured);
    failures += run("disabling shader rule preserves shortcut", testDisablingShaderRulePreservesShortcut);
    failures += run("custom shader parameters expand into page scroll",
        testCustomShaderParametersExpandIntoPageScroll);
    failures += run("renderer profile sections collapse and persist", testRendererProfileSectionsCollapseAndPersist);
    failures += run("output experiments persist and restore defaults",
        testOutputExperimentsPersistAndRestoreDefaults);
    failures += run("output diagnostic preset can inherit",
        testOutputDiagnosticPresetCanInherit);
    failures += run("Screen Config sections and inline units", testScreenConfigSectionsAndInlineUnits);
    failures += run("queue units and LUT controls use consistent rows",
        testQueueUnitsAndLutControlsUseConsistentRows);
    failures += run("active profile markers cover relevant lists", testActiveProfileMarkersCoverRelevantLists);
    failures += run("active profile opens selected after late status and preserves edits",
        testActiveProfileOpensSelectedAfterLateStatusAndPreservesEdits);
    failures += run("active shader markers use authoritative set", testActiveShaderMarkersUseAuthoritativeSet);
    failures += run("unchanged active profile status avoids list invalidation",
        testUnchangedActiveProfileStatusDoesNotInvalidateLists);
    failures += run("active shader status rejects stale generation", testActiveShaderStatusRejectsStaleGeneration);
    failures += run("standalone Config reads live active profile status", testStandaloneConfigAcceptsLiveActiveProfileStatus);
    failures += run("LUT selector discovers installation LUT files", testLutSelectorDiscoversInstallationLutFiles);
	failures += run("inherited calibration LUT uses effective control state",
		testInheritedCalibrationLutUsesEffectiveControlState);
    failures += run("choice labels and VP Renderer name", testChoiceLabelsAndVpRendererName);
    failures += run("legacy renderer visibility defaults hidden and preserves shortcuts", testLegacyRendererVisibilityDefaultsHiddenAndPreservesShortcuts);
	failures += run("legacy renderer visibility filters existing UI immediately",
		testLegacyRendererVisibilityFiltersExistingUiImmediately);
	failures += run("General input Apply preserves backend overrides",
		testGeneralInputApplyPreservesBackendOverrides);
    failures += run("DirectShow-only input persists and General can be cleared", testDirectShowOnlyInputPersistsAndGeneralCanBeCleared);
    failures += run("new action starts unconfigured", testNewActionStartsUnconfigured);
    failures += run("empty actions show an empty state", testEmptyActionsShowEmptyState);
    failures += run("missing configuration can be created from editor",
        testMissingConfigurationCanBeCreatedFromEditor);
    failures += run("Apply OK Cancel contract", testApplyOkCancelContract);
    failures += run("failed save rolls back action draft", testFailedSaveRollsBackActionDraft);   // audit 2026-10-16
    failures += run("shortcut conflicts block Apply", testShortcutConflictsBlockApply);   // audit 2026-10-16
    failures += run("Dynamic Drive page and live switches", testLedDrivePageAndLiveSwitches);
    failures += run("de-judder box presets and mask", testDejudderBoxPresetsAndMask);
    failures += run("motion slider per rule profile", testMotionSliderPerRuleProfile);
    failures += run("picture slider presets", testPictureSliderPresets);
    failures += run("shader add-ons list", testShaderAddonsList);
    failures += run("exclusive buttons: LED source and motion source", testExclusiveButtonsForLedSourceAndMotionSource);
    failures += run("temporal dither buttons", testTemporalDitherButtons);
    failures += run("interpolation buttons", testInterpolationButtons);
    failures += run("content-adaptive group", testContentAdaptiveGroup);
    failures += run("detail restore controls", testDetailRestoreControls);
    failures += run("intent remainder group", testIntentRemainderGroup);
    failures += run("queue target by frames or time", testQueueTargetByFramesOrTime);
    failures += run("queue use mode and low-latency path", testQueueUseModeAndLowLatencyPath);
    failures += run("inherited buttons and reorganised sections", testInheritedButtonsAndReorganisedSections);
    failures += run("decklink 12-bit YCbCr control", testDeckLinkYCbCr12Control);
    failures += run("renderer cards show defaults and round-trip", testRendererCardsShowDefaultsAndRoundTrip);
    failures += run("every new LED control reaches VP", testEveryNewLedControlReachesVp);
    failures += run("every new renderer control reaches VP", testEveryNewRendererControlReachesVp);
    failures += run("config keys activate the renderer and matcher settings", testConfigKeysActivateTheRendererAndMatcherSettings);
    failures += run("shader settings come from the shader file", testShaderSettingsComeFromTheShaderFile);
    failures += run("queue frame-rate rule is a plain control", testQueueFrameRateRuleIsAPlainControl);
    failures += run("queue target delay is on the queue page", testQueueTargetDelayIsOnTheQueuePage);
    failures += run("active-picture lookahead automatic when absent", testActivePictureLookaheadIsAutomaticWhenAbsent);
    failures += run("target dropdown refreshes inline", testTargetDropdownRefreshesInline);
    failures += run("remote target switch keeps window responsive",
        testRemoteTargetSwitchKeepsWindowResponsive);
    failures += run("remote-only client opens its sole discovered VP",
        testRemoteOnlyAutomaticallyOpensSoleVp);
    failures += run("no-target startup is quiet", testNoTargetStartupIsQuiet);
    failures += run("offline file still offers targets", testOfflineFileStillOffersTargets);
    failures += run("unavailable RPC can return to local file",
        testUnavailableRpcCanReturnToLocalFile);
    failures += run("remembered target does not fall back to local",
        testRememberedTargetDoesNotFallBackToLocal);
    failures += run("remote-only client has no local file target",
        testRemoteOnlyClientHasNoLocalFileTarget);
    failures += run("DirectShow-only effect does not restart Alpha",
        testDirectShowOnlyEffectDoesNotRestartAlpha);
    failures += run("invalid renderer is rejected continuously",
        testInvalidRendererIsRejectedContinuously);
    failures += run("renderer discovery outage does not block profile edits",
        testRendererDiscoveryOutageDoesNotBlockProfileEdits);
    failures += run("shortcut effects are classified live",
        testShortcutEffectsAreClassifiedLive);
    failures += run("display warning and error precedence",
        testDisplayWarningAndErrorPrecedence);
    failures += run("Video Only startup default round trips",
        testVideoOnlyStartupDefaultRoundTrips);
    failures += run("remaining effect summary precedence",
        testRemainingEffectSummaryPrecedence);
    failures += run("DPI keyboard and accessibility behavior", testDpiKeyboardAndAccessibilityBehavior);
    failures += run("primary discovery selectors open on field click",
        testPrimaryDiscoverySelectorsOpenOnFieldClick);
    failures += run("ordinary edits avoid disk validation",
        testOrdinaryEditsDoNotRequireDiskValidation);
    failures += run("warm reveal grants foreground permission",
        testWarmRevealPathGrantsForegroundPermission);
    failures += run("shader profile intent survives renderer construction",
        testShaderProfileIntentIsRetainedBeforeRendererConstruction);
    failures += run("native owner preserves Qt input and popup association",
        testNativeOwnerPreservesQtInputAndPopupAssociation);
    failures += run("real configuration dropdown remains clickable",
        testRealConfigurationDropdownRemainsClickable);
    failures += run("cold hidden association delivers pending reveal",
        testColdHiddenAssociationDeliversPendingReveal);
	failures += run("stable reveal endpoint survives hidden Qt window",
		testStableRevealEndpointSurvivesHiddenQtWindow);
    failures += run("cross-thread activation avoids reverse callback deadlock",
        testCrossThreadActivationHasNoReverseCallbackDeadlock);
    failures += run("external foreground leaves Config topmost",
        testExternalForegroundLeavesConfigTopmost);
    failures += run("synthetic presentation target clamp",
        testSyntheticPresentationTargetClamp);
    failures += run("visible presentation target update moves Config",
        testVisiblePresentationTargetUpdateMovesEditor);
    failures += run("normal window architecture has no lease polling",
        testNormalWindowArchitectureHasNoLeasePolling);
    // Audit 2026-10-17: confirmed Config editor bugs.
    failures += run("named profile falls back to built-in default", testNamedProfileFallsBackToBuiltInDefault);
    failures += run("reveal reloads an out-of-band edit", testRevealReloadsAnOutOfBandEdit);
    failures += run("save asks when the file changed on disk", testSaveAsksWhenTheFileChangedOnDisk);
    failures += run("Cancel runs every load migration", testCancelRunsEveryLoadMigration);
    failures += run("cleared choice shows the inherited value", testClearedChoiceShowsTheInheritedValue);
    failures += run("UTF-8 BOM and equals separator", testUtf8BomAndEqualsSeparator);
    failures += run("load writes nothing unrequested", testLoadWritesNothingUnrequested);
    failures += run("advanced LED settings match the parser", testAdvancedLedSettingsMatchTheParser);
    failures += run("Dynamic Drive alias section kept", testDynamicDriveAliasSectionIsKept);   // bug hunt 2
    failures += run("every schema key has a Config control", testEverySchemaKeyHasAConfigControl);
    failures += run("hunt2 startup controls write parser spellings", testHunt2StartupControlsWriteParserSpellings);
    failures += run("hunt2 subtitle detection fields", testHunt2SubtitleDetectionFields);
    failures += run("profile boolean can be unset", testProfileBooleanCanBeUnset);
    failures += run("unsaved edits ask on close and exit", testUnsavedEditsAskOnCloseAndExit);
    failures += run("explicit auto dither shows Auto", testExplicitAutoDitherShowsAuto);
    failures += run("unfocused number boxes ignore the wheel", testUnfocusedNumberBoxesIgnoreTheWheel);
    failures += run("profile rewrites are backed-up load migrations", testProfileRewritesAreBackedUpLoadMigrations);
    failures += run("inherited Picture fields and greyed render ahead", testInheritedPictureFieldsAndGreyedRenderAhead);
    failures += run("hiccup repair is separate from motion smoothing", testHiccupRepairIsSeparateFromMotionSmoothing);
    failures += run("render-ahead controls show defaults and save", testRenderAheadControlsShowDefaultsAndSave);
    failures += run("file playback controls validate, reset and save", testFilePlaybackControlsValidateResetAndSave);
    failures += run("hunt 3: latch timing fields show defaults and grey", testHunt3LatchTimingFieldsShowDefaultsAndGrey);
    failures += run("hunt 3: retired even rates say what they map to", testHunt3RetiredEvenRatesSayWhatTheyMapTo);
    failures += run("hunt 3: interpolation grain follows hiccup repair", testHunt3InterpolationGrainFollowsHiccupRepair);
    failures += run("hunt 3: content-gated profile greys classifier inputs", testHunt3ContentGatedProfileGreysClassifierInputs);
    failures += run("hunt 3: legacy queue fold targets [queue]", testHunt3LegacyQueueFoldTargetsQueueSection);
    failures += run("hunt 3: remote discard folds like a load", testHunt3RemoteDiscardFoldsLikeALoad);
    failures += RunVpvConfigTests(run, require, save);   // VP-V section (VpvConfigTests.cpp)
    if ((!testNameFilter.isEmpty() || !testNameMatches.isEmpty()) && selectedTestsRun == 0)
        return 2;
    return failures == 0 ? 0 : 1;
}
