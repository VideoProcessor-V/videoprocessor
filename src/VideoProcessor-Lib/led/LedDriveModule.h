#pragma once

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
	};

	class LedDriveModule
	{
	public:
		LedDriveModule(const ProjectorConfig& projector, const LedDriveConfig& led,
			SerialPortFactory portFactory = [] { return CreateWin32SerialPort(); });
		~LedDriveModule();
		LedDriveModule(const LedDriveModule&) = delete;
		LedDriveModule& operator=(const LedDriveModule&) = delete;

		void Start();
		// Restores full light (when it was driving the projector) and stops the
		// thread. Called by the destructor.
		void Stop();

		// Render thread, before each output frame: the source sequence of the
		// frame about to be rendered and the source frame rate (0 = unknown;
		// output frames are then assumed to be source frames). Returns the gain
		// for that frame.
		CompensationGain BeginFrame(uint64_t sequence, double sourceFps = 0.0);
		// The same with an explicit clock (tests).
		CompensationGain BeginFrameAt(uint64_t sequence, double timeMs, double sourceFps = 0.0);
		// Render thread, after a frame: the GPU pass's counts for a frame
		// (sequence 0 when unknown).
		void SubmitMeasurement(const RawMeasurement& raw, uint64_t sequence = 0);
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
		void SubmitPrediction(uint64_t sequence, const FrameStats& predicted);

		// Diagnostics and tests.
		// True while the experimental custom-dim fade to black is engaged.
		bool CustomDimEngaged() const;
		bool DrivingProjector() const;
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
		// Real-time length of one source frame (the output frame period when the
		// source rate is unknown).
		double SourceFramePeriodMs() const;
		// The LED level the gain is following: the last level the driver
		// actually wrote to the port that has had led_latency_ms (whole frames)
		// to land (dead time, no lag). Targets the driver skipped (it keeps to
		// min_command_gap_ms) never count.
		double ModelledLight() const;
		double FramePeriodMs() const;
		int LatencyFrames() const;
		size_t LookaheadDepth() const;
		// The smoothed exact / predicted ratio applied to predictions.
		double CorrectionRatio() const;

	private:
		void Run();
		bool WaitUntil(std::chrono::steady_clock::time_point when);
		double NowMs() const;
		int LatencyFramesLocked() const;
		int LatencySourceFramesLocked() const;
		CompensationGain GainLocked() const;
		FrameStats Corrected(const FrameStats& predicted) const;
		void StepCompensateLocked(uint64_t sequence, double elapsedMs);
		void StepFadeToBlackLocked(uint64_t sequence, double timeMs);

		ProjectorConfig projector;
		LedDriveConfig led;
		SerialPortFactory portFactory;
		FluxModel flux;

		mutable std::mutex mutex;
		std::condition_variable wake;
		std::thread thread;
		bool stopping = false;
		bool started = false;

		uint64_t configGeneration = 0;
		StatsBuilder statsBuilder;
		uint64_t lastMeasuredSequence = 0;
		bool haveStats = false;
		FrameStats latest;
		bool pendingCut = false;
		bool pendingFade = false;

		std::unique_ptr<IProjectorDriver> driver;
		bool live = false;
		CompensationGain gain;
		int ticks = 0;

		const std::chrono::steady_clock::time_point created;
		bool compensating = false;
		bool applyGain = false;
		CompensateController compensator;
		ControllerOutput policy;
		bool holding = false;
		double heldGain = 1.0;

		// Per-frame clock and the dead-time model: levels written to the port,
		// by the frame during which they were written.
		uint64_t frameIndex = 0;
		double lastFrameMs = -1.0;
		double framePeriodMs = 1000.0 / 60.0;
		double sourceFps = 0.0;
		struct Command { uint64_t frame; int level; };
		std::deque<Command> commands;
		int seenSends = 0;
		int modelledLevel;
		// mode: script: emulated-contrast changes, applied led_latency_ms after
		// the script made them so they land with its LED commands.
		std::deque<std::pair<double, CompensationGain>> scheduledGains;

		// Lookahead predictions by source sequence, and the correction.
		LookaheadObserver lookahead;
		std::shared_ptr<const RawTransfer> rawTransfer;
		std::map<uint64_t, FrameStats> predictions;
		size_t lookaheadDepth = 0;
		double correction = 1.0;
		bool correctionSeeded = false;

		std::string pendingLog;
		bool pendingLogSent = false;

		// Experimental custom-dim fade to black: Normal, Deep (custom dim on
		// or going down), Leaving (back up and off). LPW1 commands wait while
		// it is not Normal.
		enum class FadeToBlack { Normal, Deep, Leaving };
		FadeToBlack fadeToBlack = FadeToBlack::Normal;
		double blackSinceMs = -1.0;
	};
}
