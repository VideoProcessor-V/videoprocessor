#pragma once

#include <QMainWindow>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <ConfigurationRpcClient.h>

#include <map>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QLabel;
class QAction;
class QCheckBox;
class QComboBox;
class QHideEvent;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QProgressBar;
class QPushButton;
class QRect;
class QStackedWidget;
class QSystemTrayIcon;
class QThread;
class QTimer;
class QToolButton;
class QWindow;
class QWinEventNotifier;
class QFormLayout;

namespace ConfigEditorCore { struct ConfigDocument; }
namespace ConfigEditorPlacement
{
    QRect ClampFrameToWorkArea(const QRect& frame, const QRect& workArea);
}

class ConfigEditorWindow final : public QMainWindow
{
public:
    struct Target
    {
        QString instanceId;
        QString label;
        QString host;
        quint16 port = 41686;
        QString vpVersion;
        bool local = false;
    };

    explicit ConfigEditorWindow(QString configPath, quintptr ownerHandle = 0,
        bool testMode = false, const QStringList& testFilteredRenderers = {},
        const QStringList& testAllRenderers = {},
        const QString& remoteHost = {}, quint16 remotePort = 41686,
        const QString& remoteName = {}, bool noTarget = false,
        bool localAvailable = true, const QString& rememberedTargetId = {},
        const QString& rememberedTargetLabel = {});
    ~ConfigEditorWindow() override;
    void selectPage(int index);
    void reveal();
    void showUpdates();
    void setTargetRefresh(std::function<void()> refresh);
    void setDiscoveredTargets(const QList<Target>& targets);
    bool awaitingTarget() const { return noTarget_; }
    void finishInitialTargetSearch();
    void refreshMonitorDiscovery();
    void setActiveProfileStatusForTesting(const QString& queue,
        const QString& renderer, const QString& color, const QString& viewport,
        const QStringList& shaders, bool shaderAvailable = true,
        const QString& zoom = {}, const QString& scaling = {},
        const QString& output = {}, const QString& subtitles = {});
    void setCalibrationStatusForTesting(bool available, bool attached,
        quint64 configIdentity, const QString& configPath,
        const QString& renderer, const QString& color, bool hdrSource = false);
    void setRendererDiscoveryForTesting(const QStringList& allRenderers,
        const QStringList& filteredRenderers);
    // Test mode normally skips modal prompts; with this on, the unsaved-edit
    // and changed-on-disk prompts are shown so a test can answer them.
    void setPromptsForTesting(bool enabled) { promptsForTesting_ = enabled; }
    // The tray menu's Exit: asks about unsaved edits first.
    void requestExit();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message,
        qintptr* result) override;

private:
    QWidget* createShell();
    QWidget* createStartupPage();
    QWidget* createQueuePage();
    QWidget* createRendererPage();
	QWidget* createScalingPage();
    QWidget* createColorConfigPage();
    QWidget* createDirectShowPage();
    QWidget* createInputProcessingPage(const QString& title, const QString& description,
        const QString& section);
    QWidget* createViewportPage();
	QWidget* createZoomPage();
    QWidget* createDvPage();
    QWidget* createStandardShadersPage();
    QWidget* createNlsShadersPage();
    QWidget* createShadersSetupPage();
    QWidget* createActionsPage();
    QWidget* createShortcutsSetupPage();
    QWidget* createShortcutsPage();
    QWidget* createLogsPage();
    QWidget* createLedDrivePage();
    // Automatic calibration: the [calibration] settings and the wizard (VP runs
    // the session; this page sends RPC commands and shows its status).
    QWidget* createCalibrationPage();
    void runCalibrationCommand(const QString& command);
    void applyCalibrationStatus(const QString& text, const QString& error, const QString& command);
    QWidget* createProfilePage(const QString& title, const QString& description,
        const QString& sectionPrefix);
    QWidget* createCard(const QString& title, const QString& description, QWidget* content);
    QWidget* createPage(const QString& title, const QString& description, QWidget* body);
    QPushButton* addNavigationButton(const QString& text, int pageIndex);
    QString value(const QString& section, const QString& key, const QString& fallback = {}) const;
    QStringList profileSections(const QString& root) const;
    QString legacySubtitleHdrValue(const QString& key) const;
    QString inheritedSharedInputLabel(const QString& key) const;
    void refreshInheritedSharedInputChoices(const QString& key);
    QLineEdit* bindTextField(const QString& section, const QString& key, const QString& fallback = {});
    QComboBox* bindChoiceField(const QString& section, const QString& key,
        const QStringList& values, const QStringList& labels = {}, bool editable = false);
    QCheckBox* bindCheckField(const QString& label, const QString& section, const QString& key, bool defaultValue = false);
    void markDirty();
	void updateEffectSummary();
    void prepareRendererPopup();
    bool validateCandidate(std::wstring& error,
        bool allowActionDrafts = false) const;
    QStringList validationErrors(QStringList& fields,
        bool allowActionDrafts, bool includeCoreValidation = true) const;
    QString displayWarning() const;
    bool updateValidationState();
    void applyNativeOwner();
    void clearNativeOwner();
    void publishNativeAssociation();
    void positionForReveal();
    bool savedForegroundOnlyEnabled() const;
    void returnFocusToPresentationTarget(const char* reason);
    void applyScopedTopmost();
    void repairOrderAboveVideoProcessor(bool requireOwnerForeground);
    void removeScopedTopmost();
    bool hasActiveOwnedPopup() const;
    bool nativeOwnerIsValid() const;
    void applyChanges();
	// True when saved locally. A remote Apply runs in the background and
	// returns false; afterRemoteApply runs once it has succeeded (audit
	// 2026-10-16).
	bool saveChanges(std::function<void()> afterRemoteApply = {});
	void setRemoteApplyBusy(bool busy);
	void rebuildConfigurationShell();
    void applyRendererVisibilityFilter(bool hideLegacyRenderers);
    bool notifyVideoProcessor();
    void refreshShaderCacheStatus();
    void loadConfiguration();
    void migrateLldvInputPolicy();
    void migrateSharedRefreshRate();
	void migrateRefreshRateSwitchMode();
    void migrateSeparatedRendererProfiles();
    void migrateUnifiedColorOutputProfiles();
    void migrateCalibrationProfiles();
    void migrateViewportZoomProfiles();
    // Hunt 2 (M1/M2): legacy queue keys and profile value rewrites, once at load.
    void migrateLegacyQueueOwnership();
    void migrateProfileValueNormalisations();
    void migrateLegacyRendererName();
    // Every load-time migration, in order: construction, target switch,
    // Cancel / Discard and reloads all run the same sequence.
    void runLoadMigrations();
    // Discards the working copy and reloads the target (Cancel / Discard).
    void discardEdits();
    // Reloads a clean editor whose file changed on disk since it was loaded.
    bool reloadIfChangedOnDisk();
    bool configurationChangedOnDisk() const;
    // Save / Discard / Keep editing for unsaved edits; true when the caller
    // may go on (saved or discarded).
    bool resolveUnsavedEdits(const QString& action, bool reloadOnDiscard = true);
    bool promptsEnabled() const { return !testMode_ || promptsForTesting_; }
    void loadDiscoveryCache();
    void applyMonitorDiscovery(const QStringList& discovered);
    void setupTray();
    void refreshTargetChoices();
    QString rememberedTargetName() const;
    QString targetSearchTitleText(bool searching) const;
    void populateTargetChoices();
    void populateTrayTargets();
    bool selectAnotherTarget(const Target& target);
    void beginRemoteTargetSwitch(const Target& target);
    void beginLocalTargetSwitch(const Target& target);
    void finishLocalTargetSwitch(const Target& target,
        std::unique_ptr<ConfigEditorCore::ConfigDocument> document,
        bool loaded, const QString& error, const QStringList& devices,
        const QMap<QString, QStringList>& connections,
        const QStringList& monitors, const QStringList& filteredRenderers,
        const QStringList& allRenderers);
    void finishRemoteTargetSwitch(const Target& target,
        std::string path, std::string bytes,
        ConfigurationRpcClient::Capabilities capabilities,
        const QString& error);
    void showTargetSwitchOverlay(const QString& label);
    void hideTargetSwitchOverlay();
    void enterTargetAddress();
    void exitApplication();
    void setStatus(const QString& message, bool error = false);
    void setWarningStatus(const QString& message);
    void refreshActiveProfileIndicators();
    void installProfileContextMenu(QListWidget* list, const QString& sectionPrefix);
    // The live command for a profile row: which group and profile to select
    // in VP, whether it switches on (or off, for standard shaders) and the
    // command text. False when the row has no live command.
    bool liveProfileCommand(const QString& sectionPrefix, const QListWidgetItem* item,
        QString& group, QString& name, bool& enable, QString& label) const;
    // A visible button that switches the selected profile in VP (the same
    // command as the row's right-click menu).
    QPushButton* addLiveToggle(QListWidget* list, const QString& sectionPrefix);
    void refreshLiveToggles();
    void installActionContextMenu(QListWidget* list);
    void runTargetCommand(const QString& group, const QString& name,
        bool enabled, bool action);
    void applyActiveProfileIndicators(bool available, const QString& queue,
        const QString& renderer, const QString& color, const QString& viewport,
        const QStringList& shaders, bool shaderAvailable,
        const QString& zoom = {}, const QString& scaling = {},
        const QString& output = {}, const QString& subtitles = {});
    void refreshRendererAutoStatus();
    void refreshCalibrationControls();
    void seedCalibratedProfile(const QString& root, const QString& section);
    QMap<QString, QString> outputTransportSelections() const;
    void synchronizeLimitedTransportFlags(const QMap<QString, QString>& before, const QString& editedSection = {});
    void refreshLimitedTransportControls();

    QString configPath_;
    QString localConfigPath_;
    QString remoteHost_;
    QString remoteName_;
    QString currentInstanceId_;
    quint16 remotePort_ = 41686;
    bool noTarget_ = false;
    bool localAvailable_ = true;
    QString rememberedTargetId_;
    QString rememberedTargetLabel_;
    bool initialSearchPending_ = false;
    QString remoteLoadError_;
    QStringList remoteLuts_;
    std::unique_ptr<ConfigurationRpcClient> remoteClient_;
    struct PreparedRemoteSwitch
    {
        std::string path;
        std::string bytes;
        ConfigurationRpcClient::Capabilities capabilities;
    };
    std::unique_ptr<PreparedRemoteSwitch> preparedRemoteSwitch_;
    struct PreparedLocalSwitch
    {
        std::unique_ptr<ConfigEditorCore::ConfigDocument> document;
        bool loaded = false;
        QString error;
        QStringList devices;
        QMap<QString, QStringList> connections;
        QStringList monitors;
        QStringList filteredRenderers;
        QStringList allRenderers;
    };
    std::unique_ptr<PreparedLocalSwitch> preparedLocalSwitch_;
    bool targetSwitchPending_ = false;
    QThread* targetSwitchThread_ = nullptr;
    QWidget* targetSwitchOverlay_ = nullptr;
    std::function<void()> targetRefresh_;
    QList<Target> discoveredTargets_;
    QList<Target> targetChoices_;
    QComboBox* targetChoice_ = nullptr;
    QLabel* targetSearchTitle_ = nullptr;
    QLabel* targetSearchHelp_ = nullptr;
    QProgressBar* targetSearchProgress_ = nullptr;
    QAction* trayOpenAction_ = nullptr;
    QMenu* targetsMenu_ = nullptr;
    QString lastAutoAttemptKey_;
    qint64 lastAutoAttemptMs_ = 0;
    quintptr ownerHandle_ = 0;
    quint32 ownerProcessId_ = 0;
    bool ownerApplied_ = false;
    quintptr presentationTargetHandle_ = 0;
    quint32 presentationTargetProcessId_ = 0;
    quintptr presentationTargetAcknowledgementHandle_ = 0;
    quint32 presentationTargetAcknowledgementProcessId_ = 0;
    void* foregroundEventHook_ = nullptr;
    void* ownerOrderEventHook_ = nullptr;
    bool foregroundRepairQueued_ = false;
    bool pendingTopmostReassert_ = false;
    bool topmostReassertDeferredForPopup_ = false;
    bool scopedTopmost_ = false;
    bool explicitRevealIntent_ = false;
    bool scopedTopmostEligible_ = false;
    bool popupActivationTransition_ = false;
    bool returnFocusAfterHide_ = false;
    bool exitRequested_ = false;
    bool updateExitPending_ = false;
    bool configurationLoaded_ = false;
    bool dirty_ = false;
    bool validationValid_ = true;
    bool hasPendingMigrations_ = false;
    bool testMode_ = false;
    bool promptsForTesting_ = false;
    // [vprenderer.motion.Default] added in memory for a file without a Motion
    // profile: removed again on save while it holds no settings.
    bool seededMotionDefault_ = false;
    std::unique_ptr<ConfigEditorCore::ConfigDocument> document_;
	quintptr publishedWindowHandle_ = 0;
	std::map<std::string, std::map<std::string, std::string>> savedSnapshot_;
    QStringList captureDevices_;
    QMap<QString, QStringList> captureConnections_;
    QMap<QString, quint64> editOrder_;
    quint64 editSerial_ = 0;
    QStringList filteredRenderers_;
    QStringList allRenderers_;
    QStringList monitors_;
    QStackedWidget* pages_ = nullptr;
    QWidget* navigation_ = nullptr;
    QLabel* status_ = nullptr;
	QLabel* effectSummary_ = nullptr;
    QComboBox* monitorChoice_ = nullptr;
    QComboBox* rendererChoice_ = nullptr;
    QThread* monitorDiscoveryThread_ = nullptr;
    QTimer* activeProfileTimer_ = nullptr;
    QThread* activeProfileThread_ = nullptr;
    QThread* manualCommandThread_ = nullptr;
    // audit 2026-10-16: the RS-232 connection test and remote Apply/OK run
    // off the UI thread; the destructor waits for them.
    QThread* projectorTestThread_ = nullptr;
    QThread* remoteApplyThread_ = nullptr;
    QThread* calibrationRpcThread_ = nullptr;
    QTimer* calibrationTimer_ = nullptr;
    QString calibrationLastStep_;
    bool calibrationActive_ = false;
    bool calibrationProbeRunning_ = false;
    QStringList invalidShortcutFields_; // audit 2026-10-16
    QTimer* shaderCacheStatusTimer_ = nullptr;
    QComboBox* actionRendererTarget_ = nullptr;
    QFormLayout* rendererShortcutForm_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QWidget* configurationHost_ = nullptr;
    QLabel* shaderCacheStatus_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
    QWidget* updatePanel_ = nullptr;
	void* revealEvent_ = nullptr;
	QWinEventNotifier* revealEventNotifier_ = nullptr;
    struct ProfileListBinding
    {
        QListWidget* list;
        QString sectionPrefix;
        QLabel* selectedTitle = nullptr;
        QPushButton* liveToggle = nullptr;
        bool pendingActiveSelection = true;
        bool editSelectionTouched = false;
    };
    std::vector<ProfileListBinding> activeProfileLists_;
    bool profileSelectionReady_ = false;
    bool selectingActiveProfile_ = false;
    bool activeProfileSnapshotAvailable_ = false;
    QString subtitleHdrFallbackSelection_;
    std::function<void()> refreshSubtitleHdrFallback_;
    struct RendererAutoStatusBinding
    {
        QString sectionPrefix;
        QString key;
        QWidget* control;
        QLabel* label;
    };
    std::vector<RendererAutoStatusBinding> rendererAutoStatusBindings_;
    QString liveSourceTransfer_;
    bool liveCalibrationAvailable_ = false;
    bool liveCalibrationLutAttached_ = false;
    bool liveCalibrationHdrSource_ = false;
    quint64 liveCalibrationConfigIdentity_ = 0;
    QString liveCalibrationConfigPath_;
    QString liveCalibrationRenderer_;
    QString liveCalibrationColor_;
};
