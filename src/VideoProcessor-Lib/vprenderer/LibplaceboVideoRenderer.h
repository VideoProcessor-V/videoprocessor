#pragma once

#include <IRenderer.h>
#include <AnalysisFrame.h>
#include <ITimingClock.h>
#include <VideoConversionOverride.h>
#include <ActivePictureDecisionTimeline.h>
#include <SubtitleBoxLookahead.h>
#include <CropDiagnosticThrottle.h>
#include <RememberedEdgeReturn.h>
#include <vprenderer/BufferedPictureExpansion.h>
#include <vprenderer/AlphaConversionTimingWindow.h>
#include <vprenderer/PulldownDetector.h>
#include <vprenderer/CaptureTimestampGap.h>
#include <vprenderer/FileFrameSlotGate.h>
#include <video_frame_formatter/IVideoFrameFormatter.h>

#include <atomic>
#include <memory>
#include <string>

namespace LedDrive { class LedDriveModule; }
#include <condition_variable>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>


class ConfigFile;
struct SceneDescriptor;
struct DisplayProfile;

// In-process D3D11 renderer. HDR input is color-managed and tone-mapped to
// an SDR Rec.709 swapchain; this backend deliberately does not emit HDR.
class LibplaceboVideoRenderer final : public IVideoRenderer
{
public:
	LibplaceboVideoRenderer(
		IRendererCallback& callback,
		uint32_t rendererGeneration,
		HWND videoHwnd,
		ITimingClock* timingClock,
		bool useFrameQueue,
		size_t frameQueueMaxSize,
		VideoConversionOverride videoConversionOverride);
	~LibplaceboVideoRenderer() override;

	size_t GetConvertedQueueSize() override;
	bool OnVideoState(VideoStateComPtr& videoState) override;
	bool SupportsDynamicVideoState() const override { return true; }
	void OnVideoFrame(VideoFrame& videoFrame) override;
	// ABI 37 (VP Renderer DirectShow filter): file frames carry their own
	// presentation time and rate; capture frames keep OnVideoFrame unchanged.
	bool SupportsTimedFileFrames() const override { return true; }
	void OnTimedVideoFrame(VideoFrame& videoFrame,
		const FileFrameTiming& timing) override;
	bool WaitForFileFrameSlot(uint32_t timeoutMs) override;
	void CancelFileFrameWait() override;
	bool HasPresentedLiveFrame() const override
	{
		return m_hasPresentedLiveFrame.load(std::memory_order_acquire);
	}
	const char* PresentedLiveFrameEvidence() const override
	{
		return "swapchain-submitted";
	}
	HRESULT OnWindowsEvent(LONG_PTR param1, LONG_PTR param2) override;
	void Build() override;
	void Start() override;
	void Stop() override;
	void StopWithIngressDrain(
		const std::function<void()>& drainAfterGraphStop) override;
	void Retire() noexcept override;
	bool RetirementSucceeded() const override
	{
		return m_retirementSucceeded.load(std::memory_order_acquire);
	}
	bool FinalizeRetirementForShutdown() noexcept override;
	void Reset() override;
	void ResetLiveQueue() override;
	void SetPostStallResetTelemetrySuppressedUntil(
		uint64_t tick) override;
	void OnSize() override;
	void OnPaint() override;
	void OnDisplayChange() override;
	void SetFrameQueueMaxSize(size_t size) override;
	void SetQueueFramePolicy(size_t startupPrerollFrames,
		size_t steadyReserveFrames, bool hasSteadyReserveFrames) override;
	void SetActivePictureLookaheadFrames(size_t frames) override;
	void SetActivePictureLookaheadAutomatic(bool automatic) override;   // Bug hunt 3 F2
	void SetSceneAwareTimingCorrection(bool enabled) override;
	uint64_t SceneAwareCorrectionDropCount() const override;
	uint64_t SceneAwareCorrectionRepeatCount() const override;
	uint64_t SceneAwareDetectedCount() const override;
	bool GetSceneDetectionStatus(CString& status) const override;
	bool GetSceneTimingPrediction(double& secondsUntilCorrection,
		double& secondsUntilPlan, int& action, bool& planned) const override;
	bool GetSceneTimingLastCorrection(int& action,
		double& secondsFromDeadline, uint64_t& correctionTick) const override;
	bool SceneTimingRatesCompatible() const override;
	bool GetSceneTimingStatus(CString& status) const override;
	bool GetSceneTimingDueStatus(int& action, CString& reason) const override;
	bool SelectDisplayRule(const CString& ruleName, CString& activeRule,
		bool& rendererRestartRequired) override;
	bool SelectShaderRule(const CString& ruleName, CString& activeRule,
		bool& rendererRestartRequired) override;
	bool SelectShaderProfiles(
		const std::vector<std::string>& profileSections,
		CString& activeState, bool& rendererRestartRequired) override;
	bool GetRenderStallStatus(CString& status) const override;
	bool RefreshShaderRule(CString& activeRule,
		bool& rendererRestartRequired) override;
	std::vector<CString> ActiveShaders() const override;
	bool GetActiveShaderSections(
		std::vector<CString>& sections) const override;
	CString ActiveShaderRule() const override;
	bool ApplyApplicationState(const UnifiedProfileRuntime::Snapshot& snapshot,
		CString& activeState,
		bool& rendererRestartRequired,
		bool& liveResetRequired) override;
	bool CycleSubtitlePreviewMode(SubtitlePreviewMode& mode) override;
	void ResetSubtitlePreviewMode() override;
	size_t GetFrameQueueSize() override;
	double EntryLatencyMs() const override;
	double ExitLatencyMs() const override;
	uint64_t DroppedFrameCount() const override;
	bool GetOutputModeInfo(CString& details) const override;
	bool GetOutputContractStatus(
		RendererOutputContract::Status& status) const override;
	bool RequestRenderedOutputCapture(CString& status) override;
	bool GetDisplayLutInfo(CString& details) const override;
	bool GetDetectedPictureAspect(DetectedPictureAspect& result) const override;
	bool GetVideoIngressInfo(CString& details) const override;
	bool GetDolbyVisionLuminanceStatus(DolbyVisionLuminanceStatus& status) const override;
	bool GetContentIntentStatus(ContentIntentStatus& status) const override;
	// Report-a-problem: this DLL's DiagnosticHistory rings as JSONL (src "renderer").
	bool WriteDiagnosticHistory(VpDiagnosticHistorySink sink, void* context,
		int64_t referenceQpc) const override;
	// Intent interpreter items 2-5 (2026-10-17): motion / ambient / black / display.
	bool GetIntentStatus(IntentStatus& status) const override;
	void SetAmbientLux(double lux, bool valid, double nowSeconds) override;
	// DV next step (f): the last rendered frame's format-neutral scene
	// descriptor and display profile (SceneDescriptor.h / DisplayProfile.h),
	// for future consumers (LED look-ahead, stats). False until a frame was rendered.
	bool GetSceneDescriptor(SceneDescriptor& descriptor, DisplayProfile& display) const;
	bool GetPresentationTargetTiming(double& leadMs,
		double& captureToTargetMs) const override;
	bool GetPresentationTimingStatus(CString& status) const override;
	bool GetDejudderStatus(CString& status) const override;
	bool GetFallbacksStatus(CString& status) const override;
	void SetQueueDelayBudget(int totalMs) override;
	bool GetLedDriveStatus(CString& status) const override;
	bool GetSourceResolutionStatus(CString& status) const override;
	bool SuspendLedDriveForTest(CString& reason) override;
	bool StopLedDriveForShutdown() override;
	void ResumeLedDriveAfterTest() override;
	bool GetRenderLoad(RendererRenderLoad& load) const override;
	void SetRenderLoadDisplayRefreshRate(double refreshRateHz) override;
	bool SupportsNativeStatsOverlay() const override { return true; }
	bool SetNativeStatsOverlay(const uint8_t* pixels, size_t byteCount,
		int width, int height, int stride) override;
	bool SetNativeSweepOverlay(const uint8_t* pixels, size_t byteCount,
		int width, int height, int stride) override;
	bool SetNativeProfileOverlay(const uint8_t* pixels, size_t byteCount,
		int width, int height, int stride) override;
	bool GetConversionPerformance(double& currentUs, double& avg10s, double& max10s) const override;
	bool GetFrameRateAndPPM(double& measuredFps, int& ppmDeviation) const override;
	bool GetCalibrationOutput(Calibration::PatternOutputInfo& info) const override;
	bool SetCalibrationPattern(const Calibration::PatternRequest& request, CString& reason) override;

private:
	struct Impl;
	struct QueuedFrame
	{
		struct ConvertedPayload
		{
			std::vector<BYTE> pixels;
			VideoFrameFormatterOutputContract contract;
			DolbyVisionTunnelFrameMetadata dolby;
			double conversionCpuMs = 0.0;
			int64_t completedQpc = 0;
			// capture_path: gpu (owner 2026-10-13): the raw DeckLink frame
			// itself (kept alive here), unpacked on the GPU (GpuCaptureUnpack);
			// pixels stays empty. contract = the planes the unpack writes.
			std::shared_ptr<VideoFrame> rawFrame;
			size_t rawRowBytes = 0;
			size_t rawBytes = 0;
			VideoFrameEncoding rawEncoding = VideoFrameEncoding::UNKNOWN;
			// The capture encoding of the frame (state->videoFrameEncoding), set on
			// every payload by ConversionLoop: raw, CPU-formatted and SDR fallback
			// alike (post-merge audit 2026-10-17: the DV pair rule needs r210 for
			// non-raw SDR fallback payloads).
			VideoFrameEncoding captureEncoding = VideoFrameEncoding::UNKNOWN;
			// capture_upload: early - the raw frame already uploaded on the
			// conversion thread (an EarlyUpload::Slot; null: the render uploads).
			std::shared_ptr<void> early;
			// dv_tunnel_gpu_unpack: the raw frame is a Dolby Vision tunnel
			// picture (dolby holds its checked DM); the render unpacks it with
			// GpuDolbyVisionTunnelUnpack, RGB8 in the high (true) or low eight
			// bits of the r210 channels.
			bool dvTunnelRaw = false;
			bool dvTunnelHighAligned = false;
			bool Raw() const { return rawFrame != nullptr; }
			size_t Bytes() const { return Raw() ? rawBytes : pixels.size(); }
		};
        struct AnalysisPayload
        {
            std::mutex mutex;
            std::shared_ptr<const void> nativeOwner;
            PreparedAnalysisFrame live;
            PreparedAnalysisFrame queued;
            bool liveNative = false;
            bool live422 = false;
        };
        std::shared_ptr<AnalysisPayload> analysis = std::make_shared<AnalysisPayload>();
		VideoFrame frame;
		VideoStateComPtr state;
		uint64_t generation = 0;
		uint64_t sourceSequence = 0;
		ActivePictureFrameIdentity activePictureIdentity;
		bool activePicturePreviewAnalyzed = false;
        uint64_t activePicturePreviewDomainKey = 0;
		bool activePicturePreviewDecisionAvailable = false;
		ActivePictureFrameDecision activePicturePreviewDecision;
		ActivePictureEvidence activePicturePreviewEvidence;
		bool activePicturePreviewNearBlackEvaluated = false;
		bool activePicturePreviewNearBlack = false;
        SubtitleBoxObservation subtitleBoxObservation;
        SubtitleBoxPreview subtitleBoxPreview;
		AlphaSourceCrop::BufferedPictureExpansionProof bufferedPictureExpansion;
        GuardedRememberedEdgeReturnCertificate guardedRememberedReturn;
		int64_t callbackQpc = 0;
		// The capture hardware's arrival time on the QPC clock (0 = unknown):
		// the de-judder's phase, free of callback scheduling jitter (stream
		// robustness, owner 2026-10-14).
		int64_t arrivalQpc = 0;
		// Frames were lost just before this one (capture missed frames, an
		// overflow drop or a backlog flush): the frame before it is not paired
		// with it, and the motion history starts over (owner 2026-10-14).
		bool gapBefore = false;
		const char* gapCause = nullptr;   // why (a static string), for the log
		// The source frames the gap lost (missed capture frames, a dropped queued
		// frame); 0 = not a loss of whole frames (a pulldown lock change, a flush).
		// Interpolation's gap fill pairs across a short loss (interpolation_fill_gaps).
		uint32_t gapFrames = 0;
		// Pulldown locked (owner 2026-10-14): the film rate of this unique
		// frame (0 = the signal's rate); arrivalQpc is then its even film time.
		double filmHz = 0.0;
		// Pulldown option b: a new picture on a repeat slot (e.g. a subtitle
		// drawn by the source): the same film frame updated - shown at once,
		// at that film frame's time, never paired for motion.
		bool filmUpdate = false;
		// ABI 37: a file frame (OnTimedVideoFrame). It leaves the queue at its
		// due time (filePresentationQpc less the presentation lead) instead of
		// on queue depth; filmHz / arrivalQpc above are its film clock.
		bool fileFrame = false;
		int64_t filePresentationQpc = 0;   // 0 = due at once
		bool filePaused = false;           // shown at once and held
		bool fileMotionOff = false;        // playback rate != 1 or paused: no motion pair
		bool fileDiscontinuity = false;    // seek / step / rate change before this frame
		int64_t enqueueQpc = 0;
		std::shared_ptr<const ConvertedPayload> converted;
		bool conversionInProgress = false;
		bool conversionFailed = false;
		bool cadenceRepeat = false;
		uint64_t cadenceActionId = 0;
		uint64_t cadencePolicyGeneration = 0;
		uint64_t cadenceDetectorGeneration = 0;
		uint64_t cadencePresentationDebt = 0;
		uint32_t cadencePresentId = 0;
		double cadenceDeadlineSeconds = 0.0;
	};

	void RenderLoop();
	void ConversionLoop();
	bool HasConversionWorkLocked() const;
	std::shared_ptr<QueuedFrame::ConvertedPayload> AcquireConversionPayload();
	void StopInternal(const std::function<void()>& drainAfterGraphStop);
	void AnalyzeActivePictureLookahead(
		std::vector<QueuedFrame>& previewFrames,
		size_t queuedFutureFrames,
		uint64_t lookaheadPolicyGeneration,
		uint64_t continuityGeneration,
		size_t activePictureFrames,    // the leading frames for the active picture; the rest feed the LED only
		// The statistics observer's (LED's) own depth: frames beyond it are walked for the
		// intent observer only (2026-10-17, queue-fed intent observer).
		size_t statisticsFutureFrames = static_cast<size_t>(-1));
	void ClearQueue(const char* reason = "queue clear");
	void BeginQueueGeneration(const char* reason, bool clearStopRequest = false);
	void ClearQueueLocked(const char* reason);
	size_t PrefillTargetLocked() const;
	size_t LockedFilmQueueTarget(size_t desired) const;   // Hunt 2 (06-interp M3)
	bool CanDequeueLocked() const;
	void SetState(RendererState state);
	void ApplyViewportTarget(bool configured, double aspect,
		const char* reason);
	void UpdateFrameRateAndPPM(timingclocktime_t frameTimestamp);
	// The queue admission shared by OnVideoFrame and OnTimedVideoFrame (the
	// former's code, moved unchanged; file fields only set file frames).
	struct FileQueueFields
	{
		int64_t presentationQpc = 0;
		bool paused = false;
		bool motionOff = false;
		bool discontinuity = false;
	};
	void EnqueueFrame(VideoFrame& videoFrame, const VideoStateComPtr& frameState,
		uint64_t enqueueGeneration, int64_t callbackQpc, int64_t arrivalQpc,
		bool gapBefore, const char* gapCause, uint32_t gapFrames, double filmHz,
		bool filmUpdate, const FileQueueFields* file);
	// ABI 37: QPC ticks until the queue's head (a file frame) is due; 0 when
	// due, or when the head is not a file frame (capture: unchanged).
	int64_t FileHeadTicksUntilDueLocked() const;
	// VP-V delay budget (ADR-0020, barrier B2): QPC ticks until a capture head has waited the
	// budget's hold (the plain path's age gate); 0 when due or no budget hold applies.
	int64_t HoldTicksUntilDueLocked() const;
	// Recomputes the queue hold from the budget at this source rate (render thread).
	// sourceRateHz: the content rate the queue holds (the film rate under a pulldown lock);
	// signalRateHz: the HDMI signal's rate (one signal frame of capture transfer).
	void UpdateQueueDelayHold(double sourceRateHz, double signalRateHz);
	// The render loop's dequeue wait: CanDequeueLocked, then (file heads only)
	// the head's due time.
	void WaitForDequeueLocked(std::unique_lock<std::mutex>& lock, bool wakeOnCalibrationPattern = false);
	bool FileFrameSlotFreeLocked() const;
	void ResetFrameRateAndPPM();
	void ApplyPendingShaderSelectionLocked();
	IRendererCallback& m_callback;
	const uint32_t m_callbackGeneration;
	HWND m_videoHwnd = nullptr;
	ITimingClock* m_timingClock = nullptr;
	bool m_useFrameQueue = true;
	VideoConversionOverride m_videoConversionOverride =
		VideoConversionOverride::VIDEOCONVERSION_NONE;

	mutable std::mutex m_stateMutex;
	VideoStateComPtr m_videoState;
	std::atomic<RendererState> m_state{RendererState::RENDERSTATE_UNKNOWN};
	std::atomic_bool m_retirementSucceeded{true};
	std::string m_requestedShaderSelector;
	std::string m_requestedShaderFingerprint;   // the rules it resolved to (audit 2026-10-16)
	std::mutex m_pendingShaderMutex;
	std::string m_pendingShaderSelector;
	std::vector<std::string> m_activeShaderSections;
	bool m_activeShaderSectionsAvailable = false;
	uint64_t m_shaderRendererGeneration = 0;
	mutable uint64_t m_lastReportedShaderStatusSerial = 0;

	mutable std::mutex m_queueMutex;
	std::condition_variable m_queueChanged;
	// ABI 37: WaitForFileFrameSlot / CancelFileFrameWait (own mutex; never
	// held with m_queueMutex across a wait).
	FileFrameSlotGate m_fileSlotGate;
	std::atomic<uint64_t> m_fileTimingRejected{0};
	// The file film clock as last logged (the filter's streaming thread only).
	double m_fileLoggedFilmHz = -1.0;
	bool m_fileLoggedMotionOff = false;
	std::deque<QueuedFrame> m_frameQueue;
	ActivePictureDecisionTimeline m_activePictureTimeline;
	size_t m_frameQueueDesiredDepth = 1;
	size_t m_frameQueueMaxSize = 1;
	uint64_t m_queueGeneration = 0;
	uint64_t m_overflowLoggedGeneration = 0;
	bool m_startupPrefillPending = false;
	int64_t m_queueDepthWindowStartNs = 0;
	size_t m_queueDepthWindowMin = 0;
	size_t m_queueDepthWindowMax = 0;
	uint64_t m_queueDepthWindowDequeues = 0;
	bool m_queueDepthWindowHasSamples = false;
	bool m_stopRequested = false;
	std::thread m_renderThread;
	std::thread m_conversionThread;
	std::mutex m_conversionPoolMutex;
	std::vector<std::vector<BYTE>> m_conversionBufferPool;
	mutable std::mutex m_conversionTimingMutex;
	AlphaConversionTimingWindow m_conversionTiming;
	std::thread m_stopWorker;
	std::atomic_bool m_stopWorkerStarted{false};

	std::unique_ptr<Impl> m_impl;
	std::shared_ptr<const ConfigFile> m_rendererConfiguration;
	std::shared_ptr<const ConfigFile> m_applicationConfiguration;
	std::shared_ptr<const ConfigFile> m_buildConfiguration;
	VideoStateComPtr m_buildVideoState;
	std::string m_buildManualRule;
	std::map<std::string, std::string> m_buildManualUnifiedProfiles;
	std::atomic_bool m_implInitialized{false};
	std::atomic<double> m_entryLatencyMs{0.0};
	std::atomic<double> m_exitLatencyMs{0.0};
	std::atomic_bool m_presentationTargetTimingKnown{false};
	std::atomic<double> m_presentationTargetLeadMs{0.0};
	std::atomic<double> m_captureToPresentationTargetMs{0.0};
	// VP-V delay budget (ADR-0020): the user's one total delay (0 = none) and the hold this
	// rate gives (DelayBudget::Plan): whole frames as the queue target, the rest as a fraction.
	std::atomic<int> m_queueDelayBudgetMs{0};
	// One coherent hold (audit B2): written together by UpdateQueueDelayHold, read together under
	// m_queueHoldMutex (lock order: m_queueMutex, then m_queueHoldMutex).
	struct QueueHold
	{
		bool active = false;
		double holdMs = 0.0;
		double fraction = 0.0;
		size_t targetFrames = 0;
	};
	mutable std::mutex m_queueHoldMutex;
	QueueHold m_queueHold;
	QueueHold CurrentQueueHold() const;
	std::string m_queueHoldLogged;
	unsigned m_heldExcessRun = 0;   // consecutive held heads a frame late (AlphaQueuePolicy::ShouldShedHeldExcess)
	std::atomic<uint64_t> m_droppedFrames{0};
	std::atomic<uint64_t> m_conversionFailures{0};
	std::atomic<size_t> m_activePictureLookaheadFrames{0};
	// Bug hunt 3 F2: the look-ahead above follows the queue (no locked-film floor).
	std::atomic<bool> m_activePictureLookaheadAutomatic{false};
	uint64_t m_activePictureLookaheadLoggedGeneration = 0;
	size_t m_activePictureLookaheadLoggedAvailable = 0xff;
    CropDiagnosticThrottle m_picturePreviewDiagnostics;
    AlphaSourceCrop::BufferedExpansionShadowTrace m_fitLookaheadShadow;
	std::atomic<uint64_t> m_missingFrameStateDrops{0};
	std::atomic<uint64_t> m_renderFailureDrops{0};
	std::atomic_bool m_sceneDetectionEnabled{false};
	std::atomic<uint64_t> m_sceneDetectorGeneration{1};
	std::atomic<uint64_t> m_sceneDetectedCount{0};
	std::atomic<uint64_t> m_sceneCorrectionDropCount{0};
	std::atomic<uint64_t> m_sceneCorrectionRepeatCount{0};
	std::atomic_bool m_sceneTimingRatesCompatible{false};
	std::atomic<int> m_scenePredictedAction{0};
	std::atomic_bool m_sceneCorrectionPlanned{false};
	std::atomic<double> m_sceneSecondsUntilCorrection{0.0};
	std::atomic<double> m_sceneSecondsUntilPlan{0.0};
	std::atomic<int> m_sceneLastCorrectionAction{0};
	std::atomic<double> m_sceneLastCorrectionSecondsFromDeadline{0.0};
	std::atomic<uint64_t> m_sceneLastCorrectionTick{0};
	std::atomic<int> m_sceneTimingStatus{0};
	std::atomic<uint32_t> m_sceneTimingRateSamples{0};
	std::atomic<double> m_sceneTimingMismatchPpm{0.0};
	// One coherent due/action/reason snapshot: low two bits encode action
	// (1=drop, 2=repeat), remaining bits encode AlphaCadenceBlockReason.
	std::atomic<uint32_t> m_sceneCorrectionDueState{0};
	std::atomic<int> m_sceneDetectionStatus{0};
	std::atomic<uint64_t> m_frameCounter{0};
	std::atomic<uint64_t> m_sourceSequence{0};
	std::atomic_bool m_hasPresentedLiveFrame{false};
	// Capture-timestamp cadence diagnostics.  This intentionally mirrors the
	// DirectShow renderer measurement but is diagnostic-only: the optional
	// renderer does not feed or alter source PPM correction.
	std::atomic<timingclocktime_t> m_firstPpmFrameTimestamp{0};
	std::atomic<timingclocktime_t> m_lastPpmFrameTimestamp{0};
	std::atomic<timingclocktime_t> m_lastPpmPublishTimestamp{0};
	std::atomic<timingclocktime_t> m_expectedPpmFrameTicks{0};
	std::atomic<uint64_t> m_ppmFrameCount{0};
	std::atomic<double> m_measuredFrameRate{0.0};
	// Frames the capture missed (whole-period timestamp gaps), and the last
	// time that was logged (stream robustness, owner 2026-10-14).
	std::atomic<uint64_t> m_captureMissedFrames{0};
	// Pulldown detection (owner 2026-10-14; dejudder_pulldown, off by
	// default). The detector runs on the capture thread only; the switch is
	// refreshed from the active settings by the render loop; Reset asks for a
	// fresh start.
	std::atomic<bool> m_pulldownDetection{false};
	std::atomic<bool> m_pulldownFixes{true};   // pulldown_fixes (audit 2026-10-16)
	bool m_pulldownFixesApplied = true;        // capture thread
	// timing_continuity_fixes (Bug hunt 2026-10-17): refreshed by the render loop.
	std::atomic<bool> m_timingContinuityFixes{true};
	// cadence_class_fixes (Hunt 2, 2026-10-17): refreshed by the render loop.
	std::atomic<bool> m_cadenceClassFixes{true};
	bool m_cadenceClassFixesApplied = true;   // capture thread
	bool m_pulldownForceGap = false;          // capture thread: a toggle restarted the film clock (06-interp L1)
	// Q1 (queue stress 2026-10-18, queue_timing_fixes): stale-frame arrivals and their
	// delivery gap (capture thread).
	CaptureArrival::Tracker m_captureArrival;
	bool m_captureStaleGapPending = false;
	const char* m_captureStaleGapCause = nullptr;
	uint64_t m_captureStaleRuns = 0;
	// motion_history_fixes (Hunt 2 fix group 5): refreshed by the render loop.
	std::atomic<bool> m_motionHistoryFixes{true};
	// Hunt 2 04 L4: incoming frames dropped to preserve queued cadence repeats since the
	// last enqueued frame; the next enqueued frame carries them as a gap. Under m_queueMutex.
	uint32_t m_repeatPreservingDrops = 0;
	// queue_timing_fixes (hunt 2 group 5, 13-timing): refreshed by the render loop.
	// The beyond-target backlog age, the timeline steering in seconds, the overflow
	// drop's gap merge and the look-ahead floor of the credit-reduced target.
	std::atomic<bool> m_queueTimingFixes{true};
	// The look-ahead's queue frames (MotionAhead::QueueFramesFor) while it is wanted,
	// else 0: the floor of a render_ahead_queue_credit-reduced target (13-timing M3).
	std::atomic<size_t> m_queueTargetFloor{0};
	// Capture timestamp breaks (UpdateFrameRateAndPPM's discontinuity branch).
	std::atomic<uint64_t> m_captureTimestampBreaks{0};
	std::atomic<bool> m_pulldownResetRequested{false};
	std::atomic<int> m_pulldownCadence{0};   // (cycle << 16) | new frames per cycle; 0 = not locked
	std::atomic<uint64_t> m_pulldownSkipped{0};
	Pulldown::Detector m_pulldown;
	Pulldown::FilmClock m_pulldownClock;
	uint64_t m_pulldownSignature = 0;
	int64_t m_pulldownFilmQpc = 0;   // the last film frame's even time (QPC)
	std::atomic<ULONGLONG> m_captureMissedLogTick{0};
	std::atomic<int> m_ppmDeviation{0};
	std::atomic_bool m_hasPpmData{false};
	std::atomic<bool> m_configuredScreenActive{false};
	std::atomic<uint64_t> m_viewportRequestSerial{0};
	std::atomic<int64_t> m_viewportRequestNs{0};
	// The automatic [display_rules] selection that built the current renderer.
	// A change requests a normal renderer rebuild so output settings cannot change
	// underneath queued frames.
	std::string m_activeDisplayRule;
	// A shortcut-selected profile.  It takes precedence over [display_rules]
	// and persists across renderer reconstruction until another profile (or
	// automatic mode) is selected.
	std::string m_manualDisplayRule;
	// One manual selection per independent unified profile group. This state is
	// copied into rebuilt renderer instances only after the selection is accepted.
	std::map<std::string, std::string> m_manualUnifiedProfiles;
	// The LED drive module (brief Stage 7) lives here rather than in Impl, so
	// internal renderer resets neither restart it nor return the projector to
	// full light. Recreated only when its configuration sections change.
	std::shared_ptr<LedDrive::LedDriveModule> m_ledDrive;
	std::string m_ledDriveSignature;
	std::string m_ledDriveAliasConflict;   // [led_drive*] and [dynamic_drive*] both present (logged once)
	// RS-232 connection test: the configuration the LED drive is restarted
	// from, and whether it is stopped for a test.
	std::shared_ptr<const ConfigFile> m_ledDriveConfiguration;
	bool m_ledDriveSuspended = false;
	// VP Renderer filter (2026-10-17): file_led_holdover_ms while the applied
	// snapshot says source=file (the DirectShow filter's FileHost); 0 for the
	// capture app, whose LED drive is never parked (LedDriveHoldover.h).
	uint32_t m_ledHoldoverMs = 0;
	// gamma_boost (and the hook's knee/EOTF) as last published, for new Impls.
	double m_ledGammaBoost = 0.0;   // the gamma_boost setting (0 = off)
	double m_ledCompensationEotf = 2.2;
	double m_ledKnee = 0.08;
	double m_ledBoostKnee = 0.0;        // delta 11
	double m_ledBoostKneeWidth = 0.05;
	bool m_ledWtwPassthrough = true;   // led_wtw_passthrough
	bool m_ledBtbPassthrough = true;   // led_btb_passthrough
	double m_ledShadowLift = 0.0;
	// The Low_Latency queue as last applied (2026-10-16): kept here so a Build, which makes a
	// new Impl, starts in the right path (it was lost until the next snapshot apply).
	bool m_lowLatencyQueue = false;
	void UpdateLedDrive(const ConfigFile& configuration);
	// Automatic calibration: a patch replaces video while this is set (the render
	// loop presents it each refresh; incoming frames are dropped). The LED drive
	// is stopped (full light) for the patches and restarted after them.
	std::atomic_bool m_calibrationPatternActive{false};
	bool m_calibrationLedSuspended = false;
};

// Tests only (planner Q4): see LibplaceboVideoRenderer.cpp.
std::string LibplaceboRendererSettingsFingerprintForTest(const char* configPath);
