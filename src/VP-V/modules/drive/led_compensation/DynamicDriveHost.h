#pragma once

// WP-15 (integration batch 1): the Dynamic Drive host adapter, outer-renderer side.
//
// Everything the renderer host (LibplaceboVideoRenderer, Bill's file) used to do for the LED
// drive outside the render thread lives here: the module's lifetime across settings applies
// ([projector.default], [led_drive.default] or its alias [dynamic_drive.default], the LED
// profile file), the RS-232 connection test's suspend / resume, the calibration patterns'
// suspend / resume, the forced shutdown's stop, the file host's hold-over park, and the
// "Dynamic Drive" status line. The host keeps one call per hook point
// (docs/design/wp15-led-callsites.md). The render-thread side is DynamicDriveFrame.
//
// Threading: the methods run on the threads the host called them on before (settings apply,
// the GUI / RPC threads, the shutdown watchdog). The ones that took the renderer's state lock
// take the same lock here (the reference given at construction), with the same scope.
// VP-only (not upstreamable): it is VP's feature.

#include <QueuedFrameStatistics.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class ConfigFile;

namespace LedDrive
{
	class LedDriveModule;

	// The compensation hook's parameters from [led_drive] that apply with or without a running
	// module (gamma_boost and its toe, the gain's knee and EOTF, the BTB/WTW passthroughs and the
	// adaptive-gamma shadow lift). Defaults are the hook's built-in values.
	struct DynamicDriveParams
	{
		double gammaBoost = 0.0;          // gamma_boost (0 = off)
		double compensationEotf = 2.2;
		double knee = 0.08;
		double boostKnee = 0.0;           // delta 11
		double boostKneeWidth = 0.05;
		bool wtwPassthrough = true;       // led_wtw_passthrough
		bool btbPassthrough = true;       // led_btb_passthrough
		double shadowLift = 0.0;          // shadow_lift_ansi while adaptive_gamma is on, else 0
	};

	// What the outer host publishes to the render side (the renderer's Impl): the module, its
	// look-ahead observer on the queued-frame walk and the hook parameters. Any thread (atomics).
	// DynamicDriveFrame (render side, libplacebo) derives from it; this part has no GPU types, so
	// the outer host (VideoProcessor-Lib) and its tests need no libplacebo.
	class DynamicDriveShared
	{
	public:
		// walkObserver: the Impl's statistics observer of the queued-frame walk (the LED's
		// look-ahead is its only producer).
		explicit DynamicDriveShared(std::shared_ptr<QueuedFrameStatistics::IObserver>& walkObserver);
		virtual ~DynamicDriveShared() = default;
		DynamicDriveShared(const DynamicDriveShared&) = delete;
		DynamicDriveShared& operator=(const DynamicDriveShared&) = delete;

		void PublishModule(const std::shared_ptr<LedDriveModule>& module);
		// The module's look-ahead watches the queued-frame walk while it wants predictions (the
		// aliasing pointer keeps the module alive while the walk holds it). onlyWhenWanted: a
		// module that wants none leaves the observer as it is (else it is cleared).
		void PublishObserver(const std::shared_ptr<LedDriveModule>& module, bool onlyWhenWanted);
		// No module: both cleared.
		void Clear();
		void PublishParams(const DynamicDriveParams& params);
		DynamicDriveParams PublishedParams() const;
		std::shared_ptr<LedDriveModule> Module() const;
		// led_lookahead_ms: the LED's own walk depth (0 without a module or an observer).
		size_t WantedLookaheadFrames() const;

	protected:
		std::shared_ptr<QueuedFrameStatistics::IObserver>& walkObserver;
		std::shared_ptr<LedDriveModule> module;   // atomic access only
		std::atomic<double> gammaBoost{ 0.0 };
		std::atomic<double> compensationEotf{ 2.2 };
		std::atomic<double> knee{ 0.08 };
		std::atomic<double> boostKnee{ 0.0 };
		std::atomic<double> boostKneeWidth{ 0.05 };
		std::atomic<bool> wtwPassthrough{ true };
		std::atomic<bool> btbPassthrough{ true };
		std::atomic<double> shadowLift{ 0.0 };
	};

	// What the host knows that the adapter needs; built by the host per call (cheap).
	struct HostContext
	{
		// The render side inside the renderer's Impl (its DynamicDriveFrame); null while there
		// is no Impl.
		DynamicDriveShared* frame = nullptr;
		// Slot drive.projector_control runs (off in the Original pathway, ADR-0019, or in
		// pipeline_slots_off): the RS-232 LED/CNTR drive may be started. True without an Impl.
		bool projectorAllowed = true;
		// GetTickCount64() of the last libplacebo render start (0 = none): the connection
		// test's "video is playing" check when no module runs.
		uint64_t lastRenderStartedTick = 0;
	};

	class DynamicDriveHost
	{
	public:
		explicit DynamicDriveHost(std::mutex& rendererStateMutex);
		DynamicDriveHost(const DynamicDriveHost&) = delete;
		DynamicDriveHost& operator=(const DynamicDriveHost&) = delete;

		// Settings snapshot applied (the host passes the snapshot's "source" variable; "file" is
		// the DirectShow filter's FileHost, the only place the drive is parked between files).
		void ApplySnapshot(const std::shared_ptr<const ConfigFile>& configuration,
			const std::string& sourceKind, const HostContext& context);

		// A new Impl was built: publish the module, its look-ahead observer and the hook's
		// parameters to the render side.
		void Attach(const HostContext& context);

		// Renderer destruction (before the Impl goes): in the filter process the running drive
		// is parked for the next graph instead of returning the projector to full light.
		void ParkForHoldover(const HostContext& context);

		// RS-232 connection test (IVideoRenderer::SuspendLedDriveForTest / ResumeLedDriveAfterTest).
		bool SuspendForTest(const HostContext& context, std::string& reason);
		void ResumeAfterTest(const HostContext& context);

		// The forced shutdown's LED stop (IVideoRenderer::StopLedDriveForShutdown). Never waits
		// for the state lock: the render side's copy first, else the lock if it is free at once.
		bool StopForShutdown(const HostContext& context);

		// Calibration patterns: the drive stops at full light while patterns show; returns true
		// when a running module was stopped. ResumeAfterCalibration restarts it (no-op when the
		// patterns did not stop one).
		bool SuspendForCalibration(const HostContext& context);
		void ResumeAfterCalibration(const HostContext& context);

		// The "Dynamic Drive" status line (IVideoRenderer::GetLedDriveStatus): "off" without a module.
		std::string StatusText() const;

		// Tests.
		std::shared_ptr<LedDriveModule> Module() const { return module; }
		bool Suspended() const { return suspended; }
		uint32_t HoldoverMs() const { return holdoverMs; }
		const DynamicDriveParams& Params() const { return params; }

	private:
		void Update(const ConfigFile& configuration, const HostContext& context);
		// Takes the module out of the host and the render side (the caller holds the state lock).
		std::shared_ptr<LedDriveModule> DetachLocked(const HostContext& context);

		std::mutex& stateMutex;
		// The module lives here rather than in the Impl, so internal renderer resets neither
		// restart it nor return the projector to full light. Recreated only when its
		// configuration sections change.
		std::shared_ptr<LedDriveModule> module;
		std::string signature;
		std::string aliasConflict;   // [led_drive*] and [dynamic_drive*] both present (logged once)
		// RS-232 connection test: the configuration the drive is restarted from, and whether it
		// is stopped for a test.
		std::shared_ptr<const ConfigFile> configuration;
		bool suspended = false;
		// file_led_holdover_ms while the applied snapshot says source=file; 0 for the capture
		// app, whose drive is never parked (LedDriveHoldover.h).
		uint32_t holdoverMs = 0;
		// gamma_boost (and the hook's knee/EOTF) as last published, for new Impls.
		DynamicDriveParams params;
		// The calibration patterns stopped the drive (restarted when they end).
		bool calibrationSuspended = false;
	};
}
