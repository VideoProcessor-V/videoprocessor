#include <pch.h>
#include "DynamicDriveHost.h"

#include <VP-V/modules/drive/led_compensation/LedDriveConfig.h>
#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>
#include <VP-V/modules/drive/led_compensation/LedDriveHoldover.h>
#include <VP-V/modules/drive/projector_control/LedDriveArbiter.h>
#include <filesource/FileHostSettings.h>
#include <ConfigFile.h>
#include <DebugLog.h>

#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <iterator>

namespace LedDrive
{
	DynamicDriveShared::DynamicDriveShared(std::shared_ptr<QueuedFrameStatistics::IObserver>& walkObserverIn) :
		walkObserver(walkObserverIn)
	{
	}

	void DynamicDriveShared::PublishModule(const std::shared_ptr<LedDriveModule>& next)
	{
		std::atomic_store(&module, next);
	}

	void DynamicDriveShared::PublishObserver(const std::shared_ptr<LedDriveModule>& next, bool onlyWhenWanted)
	{
		if (next && next->WantsPredictions())
			std::atomic_store(&walkObserver, std::shared_ptr<QueuedFrameStatistics::IObserver>(next, &next->Lookahead()));
		else if (!onlyWhenWanted)
			std::atomic_store(&walkObserver, std::shared_ptr<QueuedFrameStatistics::IObserver>());
	}

	void DynamicDriveShared::Clear()
	{
		std::atomic_store(&module, std::shared_ptr<LedDriveModule>());
		std::atomic_store(&walkObserver, std::shared_ptr<QueuedFrameStatistics::IObserver>());
	}

	void DynamicDriveShared::PublishParams(const DynamicDriveParams& p)
	{
		gammaBoost.store(p.gammaBoost, std::memory_order_release);
		compensationEotf.store(p.compensationEotf, std::memory_order_release);
		knee.store(p.knee, std::memory_order_release);
		boostKnee.store(p.boostKnee, std::memory_order_release);
		boostKneeWidth.store(p.boostKneeWidth, std::memory_order_release);
		shadowLift.store(p.shadowLift, std::memory_order_release);
		wtwPassthrough.store(p.wtwPassthrough, std::memory_order_release);
		btbPassthrough.store(p.btbPassthrough, std::memory_order_release);
	}

	std::shared_ptr<LedDriveModule> DynamicDriveShared::Module() const
	{
		return std::atomic_load(&module);
	}

	size_t DynamicDriveShared::WantedLookaheadFrames() const
	{
		if (!std::atomic_load(&walkObserver))
			return 0;
		const std::shared_ptr<LedDriveModule> led = std::atomic_load(&module);
		return led ? led->WantedLookaheadFrames() : 0;
	}

	DynamicDriveParams DynamicDriveShared::PublishedParams() const
	{
		DynamicDriveParams p;
		p.gammaBoost = gammaBoost.load(std::memory_order_acquire);
		p.compensationEotf = compensationEotf.load(std::memory_order_acquire);
		p.knee = knee.load(std::memory_order_acquire);
		p.boostKnee = boostKnee.load(std::memory_order_acquire);
		p.boostKneeWidth = boostKneeWidth.load(std::memory_order_acquire);
		p.wtwPassthrough = wtwPassthrough.load(std::memory_order_acquire);
		p.btbPassthrough = btbPassthrough.load(std::memory_order_acquire);
		p.shadowLift = shadowLift.load(std::memory_order_acquire);
		return p;
	}

	DynamicDriveHost::DynamicDriveHost(std::mutex& rendererStateMutex) : stateMutex(rendererStateMutex)
	{
	}

	void DynamicDriveHost::ApplySnapshot(const std::shared_ptr<const ConfigFile>& snapshotConfiguration,
		const std::string& sourceKind, const HostContext& context)
	{
		if (!snapshotConfiguration)
			return;
		// VP Renderer filter (2026-10-17): the LED hold-over is for file playback only.
		holdoverMs = sourceKind == "file" ?
			FileSource::ReadFileHostSettings(*snapshotConfiguration).ledHoldoverMs : 0;
		configuration = snapshotConfiguration;
		if (!suspended)
			Update(*snapshotConfiguration, context);
	}

	void DynamicDriveHost::Attach(const HostContext& context)
	{
		if (!context.frame)
			return;
		context.frame->PublishModule(module);
		if (module)
			context.frame->PublishObserver(module, true);
		context.frame->PublishParams(params);
	}

	void DynamicDriveHost::ParkForHoldover(const HostContext& context)
	{
		// VP Renderer filter (2026-10-17): in the filter process (source=file) the running LED
		// drive is held for the next graph instead of returning the projector to full light; the
		// capture app never gets here with a hold-over.
		if (holdoverMs > 0 && module && !suspended)
		{
			std::shared_ptr<LedDriveModule> held = module;
			module.reset();
			if (context.frame)
				context.frame->PublishModule(std::shared_ptr<LedDriveModule>());
			ProcessLedDriveHoldover().Park(held, signature, holdoverMs);
		}
	}

	std::shared_ptr<LedDriveModule> DynamicDriveHost::DetachLocked(const HostContext& context)
	{
		std::shared_ptr<LedDriveModule> previous = module;
		module.reset();
		signature.clear();
		if (context.frame)
			context.frame->Clear();
		return previous;
	}

	void DynamicDriveHost::Update(const ConfigFile& config, const HostContext& context)
	{
		// Only [projector.default] and [led_drive.default] (or its alias [dynamic_drive.default])
		// matter; any other configuration change leaves a running module alone.
		std::string next;
		const std::string ledSection = ResolveLedDriveSection(config);
		for (const std::string& section : { std::string(PROJECTOR_SECTION), ledSection })
			if (const auto* values = config.GetSectionValues(section))
				for (const auto& value : *values)
					next += section + "." + value.first + "=" + value.second + "\n";
		// Both spellings in one file: [led_drive.*] wins; said once per change.
		const std::string conflict = LedDriveAliasConflict(config);
		if (conflict != aliasConflict && !conflict.empty())
			DebugLog::Log("Dynamic Drive: WARNING %s", conflict.c_str());
		aliasConflict = conflict;
		// An LED profile file is part of the settings: editing it applies too.
		const std::string ledProfilePath = LedProfilePath(config);
		if (!ledProfilePath.empty())
		{
			std::ifstream profile(ledProfilePath, std::ios::binary);
			next += "profile=" + std::string((std::istreambuf_iterator<char>(profile)),
				std::istreambuf_iterator<char>());
		}
		std::string validationError;
		const bool valid = Validate(config, validationError);
		// The gamma boost applies even with the module off or in log_only.
		const LedDriveConfig boostConfig = valid ? ReadLedDriveConfig(config) : LedDriveConfig();
		// The exponent is output gamma / gamma_boost, worked out per frame from the frame's
		// output transfer; the render side logs it when it changes.
		if (boostConfig.gammaBoost != params.gammaBoost)
			DebugLog::Log("Dynamic Drive: gamma boost setting %s (%.2f)",
				boostConfig.gammaBoost > 0.0 ? "on" : "off", boostConfig.gammaBoost);
		params.gammaBoost = boostConfig.gammaBoost;
		params.compensationEotf = boostConfig.compensationEotf;
		params.knee = boostConfig.knee;
		params.boostKnee = boostConfig.gammaBoostKnee;
		params.boostKneeWidth = boostConfig.gammaBoostKneeWidth;
		params.wtwPassthrough = boostConfig.wtwPassthrough;
		params.btbPassthrough = boostConfig.btbPassthrough;
		// The shadow lift is part of adaptive gamma (owner 2026-10-14).
		params.shadowLift = boostConfig.adaptiveGamma ? boostConfig.shadowLiftAnsi : 0.0;
		if (context.frame)
			context.frame->PublishParams(params);
		// VP-V: slot drive.projector_control (off in the Original pathway, ADR-0019, or at slot
		// level): the RS-232 LED/CNTR drive is not started.
		const bool projectorAllowed = context.projectorAllowed;
		if (next == signature && (module != nullptr) ==
			(valid && projectorAllowed && ReadLedDriveConfig(config).enabled))
			return;
		// A broken LED profile never stops VP: it is left out (built-in values plus the main
		// settings) and said once per change.
		std::string profileProblem;
		if (!ValidateLedProfile(config, profileProblem))
			DebugLog::Log("Dynamic Drive: WARNING LED profile ignored (%s); using the built-in values",
				profileProblem.c_str());
		// Tuning and script/compensate switches go to the running module, so the LED carries on
		// from its level instead of restoring full light.
		if (valid && module && module->Reconfigure(ReadProjectorConfig(config), ReadLedDriveConfig(config)))
		{
			signature = next;
			if (context.frame)
				context.frame->PublishObserver(module, false);
			return;
		}
		signature = next;
		std::shared_ptr<LedDriveModule> previous = module;
		module.reset();
		if (context.frame)
			context.frame->Clear();
		if (previous) previous->Stop();
		if (!valid)
		{
			DebugLog::Log("Dynamic Drive: configuration rejected (%s); module and gamma boost off",
				validationError.c_str());
			return;
		}
		const LedDriveConfig led = ReadLedDriveConfig(config);
		if (!led.enabled || !projectorAllowed)
		{
			if (previous || (led.enabled && !projectorAllowed))
				DebugLog::Log("Dynamic Drive: disabled%s", projectorAllowed ? "" :
					" (slot drive.projector_control off: Original pathway or pipeline_slots_off)");
			return;
		}
		// VP Renderer filter (2026-10-17): a drive the previous file's renderer held (same LED
		// settings) carries on; nothing is held in the capture app.
		module = ProcessLedDriveHoldover().Adopt(next);
		if (!module)
		{
			// The serial port goes through the LED drive arbiter: VideoProcessor.exe and the
			// filter never drive the projector at the same time.
			module = std::make_shared<LedDriveModule>(ReadProjectorConfig(config), led, ArbitratedSerialPortFactory());
			module->Start();
		}
		if (context.frame)
		{
			context.frame->PublishModule(module);
			// The module's look-ahead watches the queued-frame walk; the aliasing pointer keeps
			// the module alive while the walk holds it.
			context.frame->PublishObserver(module, true);
		}
	}

	bool DynamicDriveHost::SuspendForTest(const HostContext& context, std::string& reason)
	{
		reason.clear();
		std::shared_ptr<LedDriveModule> stopped;
		{
			std::unique_lock<std::mutex> guard(stateMutex);
			bool playing = false;
			if (module)
				playing = module->MsSinceLastFrame() < 2000.0;
			else if (context.frame)
			{
				const uint64_t last = context.lastRenderStartedTick;
				playing = last != 0 && GetTickCount64() - last < 2000;
			}
			if (playing)
			{
				reason = "Not run: video is playing. Stop playback and try again.";
				return false;
			}
			stopped = DetachLocked(context);
			suspended = true;
		}
		if (stopped)
		{
			stopped->Stop();
			DebugLog::Log("Dynamic Drive: stopped for the RS-232 connection test");
		}
		return true;
	}

	// Integration 2026-10-17: the forced shutdown's LED stop. The render state lock may be held
	// by the blocked retirement, so the module comes from the render loop's atomic copy (or the
	// host's when the lock is free at once). Stop() restores full light.
	bool DynamicDriveHost::StopForShutdown(const HostContext& context)
	{
		std::shared_ptr<LedDriveModule> running;
		if (context.frame)
			running = context.frame->Module();
		if (!running)
		{
			std::unique_lock<std::mutex> guard(stateMutex, std::try_to_lock);
			if (guard.owns_lock())
				running = module;
		}
		if (!running)
			return true;
		DebugLog::Log("Dynamic Drive: stopping for the forced shutdown (restoring full light)");
		// Bug hunt 2 2026-10-17: the restore's result (light, and LDMM / CNTR when sent), so the
		// caller can say "not confirmed" when it was not.
		const bool confirmed = running->Stop();
		DebugLog::Log("Dynamic Drive: stopped for the forced shutdown%s",
			confirmed ? "" : " (restore not confirmed)");
		return confirmed;
	}

	void DynamicDriveHost::ResumeAfterTest(const HostContext& context)
	{
		// A renderer created during the test (a restart, a new signal) started its own LED drive
		// while the test held the port: that drive found the port busy and runs as a dry run.
		// Restart it too (audit stage8-rs232 item 1); the dialog calls this after every test.
		std::shared_ptr<LedDriveModule> stale;
		{
			std::unique_lock<std::mutex> guard(stateMutex);
			if (!suspended && module)
				stale = DetachLocked(context);
		}
		if (stale)
			stale->Stop();   // closes the port before the new drive opens it
		std::unique_lock<std::mutex> guard(stateMutex);
		const bool wasSuspended = suspended;
		suspended = false;
		if ((wasSuspended || stale) && configuration)
		{
			Update(*configuration, context);
			DebugLog::Log(wasSuspended ? "Dynamic Drive: restarted after the RS-232 connection test" :
				"Dynamic Drive: restarted after the RS-232 connection test (renderer created during the test)");
		}
	}

	bool DynamicDriveHost::SuspendForCalibration(const HostContext& context)
	{
		// The LED drive stops (Stop() restores full light: LDMM 0, LPW1 to the ceiling, CNTR 50)
		// so no APL command moves the light between patches.
		std::shared_ptr<LedDriveModule> stopped;
		{
			std::unique_lock<std::mutex> guard(stateMutex);
			if (module)
			{
				stopped = DetachLocked(context);
				suspended = true;
				calibrationSuspended = true;
			}
		}
		if (stopped)
			stopped->Stop();
		return stopped != nullptr;
	}

	void DynamicDriveHost::ResumeAfterCalibration(const HostContext& context)
	{
		if (!calibrationSuspended)
			return;
		calibrationSuspended = false;
		ResumeAfterTest(context);
	}

	std::string DynamicDriveHost::StatusText() const
	{
		const std::shared_ptr<LedDriveModule> led = module;
		if (!led)
			return "off";
		const LedDriveConfig config = led->Config();
		const DriverStatus driver = led->ProjectorStatus();
		const CompensationGain gain = led->CurrentGain();
		const char* mode = config.mode == Mode::Compensate ? "compensate" :
			config.mode == Mode::Script ? "script" :
			config.mode == Mode::PictureOnly ? "picture_only" : "log_only";
		char text[512];
		snprintf(text, sizeof(text),
			"source %s (mode %s%s), %s, light last sent %d (confirmed %d), contrast last sent %d, commands %d, timeouts %d, "
			"picture gain %s %.3f",
			config.commandSource.empty() ? "(from mode)" : config.commandSource.c_str(), mode,
			config.mode == Mode::Script ? (config.scriptPacing ? ", script pacing" : ", tick loop") : "",
			led->DrivingProjector() ? "driving the projector" : "not driving the projector (dry run)",
			driver.lastSentLight, driver.lastLight, driver.lastSentContrast, driver.commands, driver.timeouts,
			gain.active ? "on" : "off", gain.gain);
		return text;
	}
}
