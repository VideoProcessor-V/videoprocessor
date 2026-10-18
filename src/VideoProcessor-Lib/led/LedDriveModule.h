#pragma once

#include <atomic>

#include "LedClock.h"
#include "LedController.h"
#include "LedDriveConfig.h"
#include "LedLookahead.h"
#include "LedMeasurement.h"
#include "ProjectorDriver.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

// The LED drive module at runtime: owns the projector driver and the
// controllers. The renderer touches it at three points (plus the
// compensation hook-chain entry): BeginFrame() before each frame (gain in;
// mode: compensate also steps its controller here, once per frame),
// SubmitMeasurement() after a frame (exact stats out), and the queued-frame
// walk, through Lookahead() (raw-frame predictions). mode: script runs on the
// module's own thread every tick_ms. It never stalls the renderer: every
// renderer call only takes a short lock, and serial commands are queued to
// the driver's own thread.
namespace LedDrive
{
	struct CompensationGain
	{
		// False: the compensation hook leaves the picture untouched.
		bool active = false;
		double gain = 1.0;
		GainDomain domain = GainDomain::Encoded;
		double knee = 0.08;
		double eotf = 2.2;
		// Delta 11: the scatter-veil estimate of the latest statistics (0-1;
		// the highlight level x area through shadow_lift_ansi's ramp, slewed
		// at shadow_lift_ansi_rate per second, reset on a cut). Independent of
		// `active`.
		double veil = 0.0;
	};

	class LedDriveModule
	{
	public:
		LedDriveModule(const ProjectorConfig& projector, const LedDriveConfig& led,
			SerialPortFactory portFactory = [] { return CreateWin32SerialPort(); },
			ClockPtr clock = SteadyClock());
		~LedDriveModule();
		LedDriveModule(const LedDriveModule&) = delete;
		LedDriveModule& operator=(const LedDriveModule&) = delete;

		void Start();
		// Restores full light (when it was driving the projector) and stops the
		// thread. Called by the destructor. Safe for concurrent callers (bug
		// hunt 2 2026-10-17): only the first joins the thread, later ones wait
		// until it has stopped. Returns whether the exit restore was confirmed
		// (light, and LDMM / CNTR when sent); true when there was nothing to
		// restore or the module was not running.
		bool Stop();

		// Render thread, before each output frame: the source sequence of the
		// frame about to be rendered and the source frame rate (0 = unknown;
		// output frames are then assumed to be source frames). Returns the gain
		// for that frame.
		CompensationGain BeginFrame(uint64_t sequence, double sourceFps = 0.0);
		// The same with an explicit clock (tests).
		CompensationGain BeginFrameAt(uint64_t sequence, double timeMs, double sourceFps = 0.0);
		// Render-ahead (docs/VP-render-ahead-design.md step 6): a frame rendered aheadMs
		// before it is displayed; its time is the predicted display time (now + aheadMs)
		// and the sends' landing (led_landing_ms) stays on the wall clock.
		CompensationGain BeginFrameAhead(uint64_t sequence, double aheadMs, double sourceFps = 0.0);
		// Render thread, after a frame: the GPU pass's counts for a frame
		// (sequence 0 when unknown).
		void SubmitMeasurement(const RawMeasurement& raw, uint64_t sequence = 0);
		// Delta 11 (tests): the veil estimate without a frame step.
		double Veil() const { std::lock_guard<std::mutex> guard(mutex); return gain.veil; }
		// The gain BeginFrame last returned, without stepping anything.
		CompensationGain CurrentGain() const;

		// Lookahead (mode: compensate only). The renderer subscribes Lookahead()
		// to its queued-frame walk and hands over the raw-to-output transfer
		// whenever its colour mapping changes.
		QueuedFrameStatistics::IObserver& Lookahead() { return lookahead; }
		bool WantsPredictions() const;
		void SetRawTransfer(std::shared_ptr<const RawTransfer> transfer);
		std::shared_ptr<const RawTransfer> CurrentRawTransfer() const;
		void SetLookaheadDepth(size_t futureFrames);
		// Render thread: the captured signal's range as the picture uses it
		// (source_range: 1 full, 0 limited, -1 from the format), for the
		// look-ahead's reading of the captured frame.
		void SetSourceRange(int range) { sourceRange.store(range, std::memory_order_relaxed); }
		int SourceRange() const { return sourceRange.load(std::memory_order_relaxed); }
		void SubmitPrediction(uint64_t sequence, const FrameStats& predicted);
		// Hunt 3 (LED2): a new stream starts on this module (a hold-over drive
		// adopted by the next file, whose source sequence restarts): the
		// predictions by sequence are dropped and the look-ahead observer
		// starts over (sequence, persistence windows, clock) at its next frame,
		// so the new file is looked ahead from its first frame. Called by the
		// adopting renderer; BeginFrame also calls it when the source sequence
		// goes backwards. Any thread.
		void ResetLookahead();
		uint64_t LookaheadEpoch() const { return lookaheadEpoch.load(std::memory_order_acquire); }
		// Tests: the stored prediction for a source sequence (uncorrected).
		bool PredictionFor(uint64_t sequence, FrameStats& out) const;
		// Hunt 3: the most predictions kept (sequence 0 never prunes them).
		static constexpr size_t MAX_PREDICTIONS = 4096;

		// Diagnostics and tests.
		// True while the experimental custom-dim fade to black is engaged.
		bool CustomDimEngaged() const;
		bool DrivingProjector() const;
		// Milliseconds since the last BeginFrame (a video frame), on the
		// module's own clock; very large before the first. The RS-232 test is
		// refused while video plays; led_idle_restore_s acts on it.
		double MsSinceLastFrame() const;
		DriverStatus ProjectorStatus() const;
		int Ticks() const;
		// Settings changed while running: applies them to the running module
		// (the LED and both controllers carry on from where they are) and
		// returns true; false when the change needs a new module - the
		// connection settings differ, or the module would start or stop
		// driving the projector (enabled, log_only).
		bool Reconfigure(const ProjectorConfig& projector, const LedDriveConfig& led);
		// Increases with every Reconfigure().
		uint64_t ConfigGeneration() const;
		LedDriveConfig Config() const;
		// The gamma VP's output is encoded for (the renderer sets it from the
		// frame's output transfer): the gamma boost's base (delta 4).
		void SetDisplayGamma(double gamma) { displayGamma.store(gamma, std::memory_order_release); }
		double DisplayGamma() const { return displayGamma.load(std::memory_order_acquire); }
		// Real-time length of one source frame (the output frame period when the
		// source rate is unknown).
		double SourceFramePeriodMs() const;
		// The LED level the gain is following: the last level the driver
		// actually wrote to the port that has had led_latency_ms (whole frames)
		// to land, then settling with led_settle_ms (7c-1; 0 = dead time only),
		// as the equivalent (fractional) level. Targets the driver skipped (it
		// keeps to min_command_gap_ms) never count.
		double ModelledLight() const;
		// mode: compensate's CNTR assist (7c-1): the CNTR value it last asked
		// for, and the modelled CNTR light ((CNTR / 50)^E, after
		// cntr_latency_ms and cntr_settle_ms) the gain divides by.
		int AssistContrast() const;
		double ModelledContrastLight() const;
		// 7c-2: the bright cut's fade-up is running, and its displayed light.
		bool Recovering() const;
		double RecoveryDisplayed() const;
		double FramePeriodMs() const;
		int LatencyFrames() const;
		// Bug hunt 2 (render-ahead): the source frames after the current one that a
		// command written now lands on (led_latency_ms less the frame's lead).
		int CommandLatencySourceFrames() const;
		size_t LookaheadDepth() const;
		// led_lookahead_ms (review 2026-10-17) in source frames at the measured
		// rate; 0 = the queue walk's own depth (active_picture_lookahead_frames).
		// For the renderer: how far its queued-frame walk would have to reach
		// for the LED alone (wired 2026-10-17: the walk visits max(active picture, this),
		// the frames beyond the active picture feed the LED only).
		// Owner 2026-10-17 (queue = look-ahead): WHOLE_QUEUE (every queued frame)
		// whenever the LED looks ahead (compensate with lookahead, internal_script
		// with script_lookahead), or led_lookahead_ms's frames when set; 0 otherwise.
		size_t WantedLookaheadFrames() const;
		static constexpr size_t WHOLE_QUEUE = static_cast<size_t>(-1);
		// The smoothed exact / predicted ratio applied to predictions.
		double CorrectionRatio() const;

	private:
		std::atomic<double> displayGamma{ 2.2 };
		void Run();
		bool WaitUntil(std::chrono::steady_clock::time_point when);
		// The same, also ending (false) as soon as the settings change from
		// `generation`, so mode: script stops its tick at once.
		bool WaitUntil(std::chrono::steady_clock::time_point when, uint64_t generation);
		double NowMs() const;
		int LatencyFramesLocked() const;
		int LatencySourceFramesLocked() const;
		// Bug hunt 2: a queued, not yet written LPW1's landing, in ms from the
		// current frame (the driver's next send slot + led_latency_ms - the lead).
		double QueuedLightLandingMsLocked(const DriverStatus& status) const;
		CompensationGain GainLocked() const;
		void ConfigureModelsLocked();
		double LedFractionLocked(double level) const;
		int LatencyFramesForLocked(int ms) const;
		FrameStats Corrected(const FrameStats& predicted) const;
		void StepCompensateLocked(uint64_t sequence, double elapsedMs);
		void StepFadeToBlackLocked(uint64_t sequence, double timeMs);
		// audit 2026-10-16: led_idle_restore_s.
		double MsSinceLastFrameLocked() const;
		void RestoreIdleCompensateLocked();

		// The module's and its driver's time source (the steady clock; a manual
		// clock in tests). Declared before `created`, which reads it.
		const ClockPtr clock;
		ProjectorConfig projector;
		LedDriveConfig led;
		SerialPortFactory portFactory;
		FluxModel flux;

		mutable std::mutex mutex;
		std::condition_variable wake;
		std::thread thread;
		bool stopping = false;
		bool started = false;
		// Stop() (bug hunt 2): signalled when the joining caller has finished,
		// for concurrent callers; the exit restore's result, written by the
		// module's thread before it ends.
		std::condition_variable stopped;
		bool restoreConfirmed = true;
		uint64_t stopGeneration = 0;     // completed Stop()s
		bool stoppedConfirmed = true;    // restoreConfirmed as the last Stop() saw it

		uint64_t configGeneration = 0;
		StatsBuilder statsBuilder;
		uint64_t lastMeasuredSequence = 0;
		bool haveStats = false;
		FrameStats latest;
		bool veilValid = false;   // delta 11: the veil estimate has a value
		bool pendingCut = false;
		bool pendingFade = false;

		std::unique_ptr<IProjectorDriver> driver;
		bool live = false;
		CompensationGain gain;
		int ticks = 0;

		const std::chrono::steady_clock::time_point created;
		// Hunt 3 (LED5): written under the mutex, but also read without it on the
		// LED thread (smoothing, the dry-run logs): atomic, so no read races a write.
		std::atomic<bool> compensating{ false };
		std::atomic<bool> applyGain{ false };
		CompensateController compensator;
		ControllerOutput policy;
		bool holding = false;
		double heldGain = 1.0;

		// Per-frame clock and the dead-time model: levels written to the port,
		// by the frame during which they were written.
		uint64_t frameIndex = 0;
		double lastFrameMs = -1.0;
		// audit 2026-10-16: NowMs() at the last BeginFrame (-1 before any),
		// and the idle restore (led_idle_restore_s) done since it.
		double lastFrameWallMs = -1.0;
		// Render-ahead: how far ahead of the wall clock the next / the last / the
		// previous frame's time is (0 = rendered when displayed, as before).
		double nextFrameAheadMs = 0.0;
		double frameAheadMs = 0.0;
		double previousFrameAheadMs = 0.0;
		bool idleRestored = false;
		double framePeriodMs = 1000.0 / 60.0;
		double sourceFps = 0.0;
		// landMs (led_landing_ms, review 2026-10-17): when it lands on the
		// frame clock (the driver's write time + led_latency_ms); -1 = by
		// frames (the switch off, or no write time from the driver).
		struct Command { uint64_t frame; int level; double landMs; };
		std::deque<Command> commands;
		int seenSends = 0;
		// Bug hunt 2: the LPW1 target last queued to the driver and not yet seen
		// written (-1 none); a landing in LedLandingsLocked until it is written.
		int queuedLight = -1;
		int modelledLevel;
		// 7c-1: the LED's light after the dead time, settling (as a fraction
		// of the ceiling's flux), and the same for CNTR (its light factor).
		SettleModel ledModel;
		struct ContrastCommand { uint64_t frame; int value; double landMs; };
		std::deque<ContrastCommand> contrastCommands;
		int seenContrastSends = 0;
		SettleModel contrastModel;
		CntrPairing pairing;
		// Compensate has sent a CNTR other than 50: the exit, or a switch to
		// another mode, puts it back to 50.
		bool assistContrastDriven = false;
		// The pairing was allowed to command CNTR on the previous frame.
		bool assistAllowed = false;
		// 7c-2: the gain-shaped fade-up after a bright cut, its start (the
		// cut frame's source sequence; 0 = at once) and this frame's limit.
		CutRecovery recovery;
		bool pendingRecovery = false;
		uint64_t pendingRecoverySequence = 0;
		double pendingRecoveryCeiling = 1.0;
		double recoveryLimit = 1e9;
		uint64_t lastBrightCutSequence = 0;
		int DmdClipLocked() const;
		int CutLevelLocked() const;
		// cntr_pair_predict (post-merge audit 2026-10-17): the LPW1s still to land, as
		// (ms from the current frame, LED fraction) in time order - those in flight and
		// pendingLevel (this tick's, not yet written; -1 none) or else the queued
		// queuedLight, led_latency_ms after the driver's next send slot (bug hunt 2).
		std::vector<std::pair<double, double>> LedLandingsLocked(int pendingLevel) const;
		void HoldContrastLocked();
		// mode: script with script_smoothing: on (step 7b): VP's gain bridging
		// the projector's whole CNTR steps; smoothing is set by the script
		// thread while it applies.
		ScriptSmoother smoother;
		bool smoothing = false;
		// A live switch from script to compensate: compensate waits (picture
		// untouched, no LPW1) until the script thread has put CNTR back to 50
		// (audit stage7-step7ab, Should fix 6).
		bool awaitingContrastReset = false;

		// Lookahead predictions by source sequence, and the correction.
		LookaheadObserver lookahead;
		std::shared_ptr<const RawTransfer> rawTransfer;
		std::map<uint64_t, FrameStats> predictions;
		std::atomic<uint64_t> lookaheadEpoch{ 0 };
		void ResetLookaheadLocked();
		std::atomic<int> sourceRange{ -1 };
		size_t lookaheadDepth = 0;
		// Review 2026-10-17: NowMs() of the last walk (SetLookaheadDepth; -1
		// before any), and the depth-0 warning's state (-1 not yet decided,
		// 0 depth available, 1 warned), logged by the module's thread.
		double lastWalkWallMs = -1.0;
		int lookaheadWarnState = -1;
		// Owner 2026-10-17: NowMs() of the last prediction (-1 before any).
		double lastPredictionWallMs = -1.0;
		// The source sequence of the last BeginFrame (lastFrameMs is its time).
		uint64_t lastFrameSequence = 0;
		// The LED looks ahead in the current mode (compensate: lookahead;
		// internal_script: script_lookahead).
		bool LookaheadWantedLocked() const;
		// The walk's future frames the LED may use now: 0 when no walk came for a
		// second (Low Latency queues nothing), capped by led_lookahead_ms.
		size_t LookaheadDepthLocked() const;
		// The once-per-change look-ahead log (pendingWarning): a warning only for a
		// real fault - frames queued and walked but no statistics from them.
		void UpdateLookaheadWarningLocked();
		std::string pendingWarning;
		size_t LedLookaheadFramesLocked() const;
		// led_landing_ms: the frame-clock time a send written at `sentAt`
		// lands (+latencyMs), or -1 when it lands by frames.
		double LandingMsLocked(std::chrono::steady_clock::time_point sentAt, int latencyMs,
			double timeMs, double previousFrameMs, double previousWallMs) const;
		bool LandsLocked(uint64_t frame, double landMs, int latencyFrames, double timeMs) const;
		double correction = 1.0;
		bool correctionSeeded = false;
		// The output frame of the last correction update (audit 2026-10-16).
		uint64_t lastCorrectionFrame = 0;

		std::string pendingLog;
		bool pendingLogSent = false;

		// Experimental custom-dim fade to black: Normal, Deep (custom dim on
		// or going down), Leaving (back up and off). LPW1 commands wait while
		// it is not Normal.
		enum class FadeToBlack { Normal, Deep, Leaving };
		FadeToBlack fadeToBlack = FadeToBlack::Normal;
		double blackSinceMs = -1.0;
		// The look-ahead reached the frame the last command lands on.
		bool lookaheadCovers = false;
	};
}
