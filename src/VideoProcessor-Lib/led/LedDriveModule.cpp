#include <pch.h>
#include "LedDriveModule.h"

#include "../DebugLog.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace LedDrive
{
	namespace
	{
		const char* ModeName(Mode mode)
		{
			return mode == Mode::Compensate ? "compensate" : mode == Mode::Script ? "script" : "log_only";
		}

		// Predictions are kept this many frames behind the current one, long
		// enough for the exact measurement of the same frame to come back.
		constexpr uint64_t PREDICTION_HISTORY = 64;
		// The correction ratio's smoothing window.
		constexpr double CORRECTION_WINDOW_MS = 3000.0;

		double Scaled(double code, double ratio)
		{
			return std::max(0.0, std::min(16.0 + (code - 16.0) * ratio, 255.0));
		}

		using Commands = std::vector<std::pair<std::string, int>>;
		const char* const CUSTOM_CHANNELS[3] = { "LDCR", "LDCG", "LDCB" };

		// Into the custom dim: all three colours to the value that matches
		// LPW1 at the fade-to-black floor, custom mode on, then down - in one
		// move for all three, or the colours taking turns in small steps so
		// they are never more than one step apart.
		Commands CustomDimDown(const LedDriveConfig& led)
		{
			Commands commands;
			for (const char* channel : CUSTOM_CHANNELS) commands.push_back({ channel, led.ftbCustomStart });
			commands.push_back({ "LDMM", 1 });
			if (led.ftbCustomDim == FtbCustomDim::Turns)
			{
				for (int level = led.ftbCustomStart - led.ftbTurnStep; ; level -= led.ftbTurnStep)
				{
					level = std::max(level, led.ftbCustomFloor);
					for (const char* channel : CUSTOM_CHANNELS) commands.push_back({ channel, level });
					if (level == led.ftbCustomFloor) break;
				}
			}
			else
				for (const char* channel : CUSTOM_CHANNELS) commands.push_back({ channel, led.ftbCustomFloor });
			return commands;
		}

		// Out again as the picture returns: one command, standard dim mode,
		// back on LPW1 (the colour values are set again on the next entry).
		Commands CustomDimUp(const LedDriveConfig&)
		{
			return { { "LDMM", 0 } };
		}
	}

	LedDriveModule::LedDriveModule(const ProjectorConfig& projectorIn, const LedDriveConfig& ledIn,
		SerialPortFactory portFactoryIn)
		: projector(projectorIn), led(ledIn), portFactory(std::move(portFactoryIn)),
		flux(projectorIn.fluxTable), statsBuilder(ledIn), created(std::chrono::steady_clock::now()),
		compensator(ledIn, flux), modelledLevel(ledIn.ledCeiling), lookahead(*this),
		rawTransfer(std::make_shared<const RawTransfer>(IdentityRawTransfer()))
	{
		gain.knee = led.knee;
		gain.eotf = led.compensationEotf;
		compensator.Reset(led.ledCeiling);
	}

	LedDriveModule::~LedDriveModule()
	{
		Stop();
	}

	double LedDriveModule::NowMs() const
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - created).count();
	}

	int LedDriveModule::LatencyFramesLocked() const
	{
		return static_cast<int>(std::lround(projector.ledLatencyMs / framePeriodMs));
	}

	// led_latency_ms in source frames, for finding the queued frame a command
	// is for (predictions are keyed by source sequence; 24p film in a 60 Hz
	// output shows each source frame for 2-3 output frames).
	int LedDriveModule::LatencySourceFramesLocked() const
	{
		return sourceFps > 0.0 ? static_cast<int>(std::lround(projector.ledLatencyMs * sourceFps / 1000.0)) :
			LatencyFramesLocked();
	}

	double LedDriveModule::SourceFramePeriodMs() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return sourceFps > 0.0 ? 1000.0 / sourceFps : framePeriodMs;
	}

	bool LedDriveModule::WantsPredictions() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return led.mode == Mode::Compensate;
	}

	LedDriveConfig LedDriveModule::Config() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return led;
	}

	uint64_t LedDriveModule::ConfigGeneration() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return configGeneration;
	}

	namespace
	{
		// Everything the driver was built from except the two values the
		// module reads live (latency, flux table).
		bool SameConnection(const ProjectorConfig& a, const ProjectorConfig& b)
		{
			return a.protocol == b.protocol && a.port == b.port && a.baud == b.baud &&
				a.lightMin == b.lightMin && a.lightMax == b.lightMax && a.ackTimeoutMs == b.ackTimeoutMs &&
				a.minCommandGapMs == b.minCommandGapMs && a.maxLevelsPerCommand == b.maxLevelsPerCommand &&
				a.burstCommands == b.burstCommands && a.burstPauseMs == b.burstPauseMs &&
				a.readBack == b.readBack && a.echo == b.echo && a.terminator == b.terminator;
		}
	}

	bool LedDriveModule::Reconfigure(const ProjectorConfig& projectorIn, const LedDriveConfig& ledIn)
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (!ledIn.enabled || !SameConnection(projector, projectorIn) ||
			EffectiveProtocol(projector, led) != EffectiveProtocol(projectorIn, ledIn))
			return false;
		const bool wasCompensating = led.mode == Mode::Compensate;
		projector = projectorIn;
		led = ledIn;
		flux = FluxModel(projector.fluxTable);
		statsBuilder = StatsBuilder(led);
		compensator.Reconfigure(led, flux);
		gain.knee = led.knee;
		gain.eotf = led.compensationEotf;
		if (started && driver)
		{
			const bool nowCompensating = led.mode == Mode::Compensate;
			if (nowCompensating && !wasCompensating)
			{
				// From script: carry on from the LED's current level.
				const DriverStatus status = driver->Status();
				compensator.Reset(status.lastSentLight >= 0 ? status.lastSentLight : modelledLevel);
				seenSends = status.lightSends;
				scheduledGains.clear();
				policy = ControllerOutput();
				holding = false;
			}
			if (!nowCompensating && wasCompensating)
			{
				gain.active = false;
				gain.gain = 1.0;
				holding = false;
			}
			compensating = nowCompensating;
			applyGain = live && led.mode != Mode::LogOnly;
		}
		++configGeneration;
		wake.notify_all();
		DebugLog::Log("LED drive: settings applied to the running module (mode=%s); the LED carries on from its current level",
			ModeName(led.mode));
		return true;
	}

	double LedDriveModule::ModelledLight() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return modelledLevel;
	}

	double LedDriveModule::FramePeriodMs() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return framePeriodMs;
	}

	int LedDriveModule::LatencyFrames() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return LatencyFramesLocked();
	}

	size_t LedDriveModule::LookaheadDepth() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return lookaheadDepth;
	}

	double LedDriveModule::CorrectionRatio() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return correction;
	}

	void LedDriveModule::Start()
	{
		std::lock_guard<std::mutex> guard(mutex);
		if (started) return;
		started = true;
		stopping = false;
		thread = std::thread(&LedDriveModule::Run, this);
	}

	void LedDriveModule::Stop()
	{
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (!started) return;
			stopping = true;
		}
		wake.notify_all();
		if (thread.joinable()) thread.join();
		std::lock_guard<std::mutex> guard(mutex);
		started = false;
	}

	void LedDriveModule::SubmitMeasurement(const RawMeasurement& raw, uint64_t sequence)
	{
		std::lock_guard<std::mutex> guard(mutex);
		// A source frame shown again (24p in a 60 Hz output) adds nothing and
		// would hide fades (APL "unchanged" on every repeat).
		if (sequence != 0 && sequence == lastMeasuredSequence) return;
		lastMeasuredSequence = sequence;
		const FrameStats stats = statsBuilder.Build(raw, NowMs());
		if (stats.blocks == 0) return;
		latest = stats;
		haveStats = true;
		// A cut or fade between two controller steps must still reach it.
		pendingCut = pendingCut || stats.cut;
		pendingFade = pendingFade || stats.fade;
		// Self-correction: the exact APL of this frame against what the raw
		// path predicted for it, smoothed over a few seconds.
		const auto predicted = sequence != 0 ? predictions.find(sequence) : predictions.end();
		if (predicted != predictions.end() && predicted->second.apl > 20.0 && stats.apl > 17.0)
		{
			const double ratio = std::max(0.25, std::min((stats.apl - 16.0) / (predicted->second.apl - 16.0), 4.0));
			const double weight = std::min(1.0, framePeriodMs / CORRECTION_WINDOW_MS);
			correction = correctionSeeded ? correction + (ratio - correction) * weight : ratio;
			correctionSeeded = true;
		}
	}

	void LedDriveModule::SetRawTransfer(std::shared_ptr<const RawTransfer> transfer)
	{
		if (!transfer || transfer->size() != RAW_TRANSFER_SIZE) return;
		std::lock_guard<std::mutex> guard(mutex);
		rawTransfer = std::move(transfer);
		// A new profile: the old correction no longer applies.
		correction = 1.0;
		correctionSeeded = false;
	}

	std::shared_ptr<const RawTransfer> LedDriveModule::CurrentRawTransfer() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return rawTransfer;
	}

	void LedDriveModule::SetLookaheadDepth(size_t futureFrames)
	{
		std::lock_guard<std::mutex> guard(mutex);
		lookaheadDepth = futureFrames;
	}

	void LedDriveModule::SubmitPrediction(uint64_t sequence, const FrameStats& predicted)
	{
		std::lock_guard<std::mutex> guard(mutex);
		predictions[sequence] = predicted;
	}

	FrameStats LedDriveModule::Corrected(const FrameStats& predicted) const
	{
		if (correction == 1.0) return predicted;
		FrameStats stats = predicted;
		stats.apl = Scaled(predicted.apl, correction);
		stats.highlight = Scaled(predicted.highlight, correction);
		stats.sustainedApl = Scaled(predicted.sustainedApl, correction);
		stats.sustainedHighlight = Scaled(predicted.sustainedHighlight, correction);
		stats.histogram.fill(0);
		for (int code = 0; code < HISTOGRAM_BINS; ++code)
			stats.histogram[static_cast<int>(Scaled(code, correction))] += predicted.histogram[code];
		return stats;
	}

	CompensationGain LedDriveModule::BeginFrame(uint64_t sequence, double sourceFpsIn)
	{
		return BeginFrameAt(sequence, NowMs(), sourceFpsIn);
	}

	CompensationGain LedDriveModule::BeginFrameAt(uint64_t sequence, double timeMs, double sourceFpsIn)
	{
		std::lock_guard<std::mutex> guard(mutex);
		sourceFps = sourceFpsIn > 0.0 && sourceFpsIn < 1000.0 ? sourceFpsIn : 0.0;
		double elapsed = framePeriodMs;
		if (lastFrameMs >= 0.0)
		{
			elapsed = std::max(1.0, std::min(timeMs - lastFrameMs, 250.0));
			framePeriodMs = std::max(4.0, std::min(framePeriodMs + (elapsed - framePeriodMs) * 0.1, 100.0));
		}
		lastFrameMs = timeMs;
		// Levels the driver wrote since the last frame: they went out during
		// that frame.
		if (compensating && applyGain && driver)
		{
			const DriverStatus status = driver->Status();
			if (status.lightSends != seenSends && status.lastSentLight >= 0)
				commands.push_back({ frameIndex, status.lastSentLight });
			seenSends = status.lightSends;
		}
		++frameIndex;
		while (!scheduledGains.empty() && scheduledGains.front().first <= timeMs)
		{
			gain = scheduledGains.front().second;
			scheduledGains.pop_front();
		}
		if (compensating && haveStats)
		{
			StepCompensateLocked(sequence, elapsed);
			StepFadeToBlackLocked(sequence, timeMs);
		}
		// Dead time: the LED reaches a commanded level led_latency_ms (in
		// whole frames) after the command, and the gain switches with it.
		const int latency = LatencyFramesLocked();
		while (!commands.empty() && commands.front().frame + latency <= frameIndex)
		{
			modelledLevel = commands.front().level;
			commands.pop_front();
		}
		if (holding && !policy.holdGain) holding = false;
		if (policy.holdGain && !holding)
		{
			heldGain = CompensationGainFor(flux, led.ledCeiling, modelledLevel, policy);
			holding = true;
		}
		while (!predictions.empty() && predictions.begin()->first + PREDICTION_HISTORY < sequence)
			predictions.erase(predictions.begin());
		return GainLocked();
	}

	void LedDriveModule::StepCompensateLocked(uint64_t sequence, double elapsedMs)
	{
		// The frame a command sent now is for: the one shown when the LED gets
		// there. Its predicted statistics when the walk has seen it (or the
		// furthest it has seen); otherwise the latest exact ones. Predictions
		// are by source frame, so the latency is counted in source frames.
		const int latency = LatencySourceFramesLocked();
		FrameStats target = latest;
		target.cut = pendingCut;
		target.fade = pendingFade;
		const auto first = predictions.upper_bound(sequence);
		const auto forCommand = predictions.upper_bound(sequence + static_cast<uint64_t>(latency));
		const bool predicted = sequence != 0 && first != forCommand;
		// The look-ahead reaches the frame the LED will land on.
		const bool covered = predicted && std::prev(forCommand)->first >= sequence + static_cast<uint64_t>(latency);
		if (predicted)
		{
			target = Corrected(std::prev(forCommand)->second);
			for (auto it = first; it != forCommand; ++it) target.cut = target.cut || it->second.cut;
		}
		pendingCut = pendingFade = false;
		// The furthest queued frame, for early ramps.
		LedDrive::Lookahead ahead;
		bool haveAhead = false;
		const auto horizon = predictions.upper_bound(sequence + lookaheadDepth);
		if (sequence != 0 && first != horizon)
		{
			const auto furthest = std::prev(horizon);
			ahead.stats = Corrected(furthest->second);
			ahead.framesAhead = static_cast<int>(furthest->first - sequence);
			for (auto it = first; it != horizon; ++it) ahead.cutAhead = ahead.cutAhead || it->second.cut;
			haveAhead = true;
		}
		const ControllerOutput out = compensator.Tick(target, elapsedMs, haveAhead ? &ahead : nullptr,
			predicted, covered);
		lookaheadCovers = covered;
		// Queued to the driver's thread, which keeps only the newest and
		// sends it when the command gap allows; the model follows what it
		// actually sends (above).
		if (out.lightCommand >= 0 && applyGain && fadeToBlack != FadeToBlack::Deep)
		{
			if (out.jump) driver->SetLightJump(out.lightCommand);
			else driver->SetLight(out.lightCommand);
		}
		policy = out;
		gain.knee = out.knee;
		++ticks;
		if (out.lightCommand >= 0 || pendingLog.empty())
		{
			pendingLog = out.log;
			pendingLogSent = out.lightCommand >= 0;
		}
	}

	void LedDriveModule::StepFadeToBlackLocked(uint64_t sequence, double timeMs)
	{
		if ((led.ftbCustomDim == FtbCustomDim::Off && fadeToBlack == FadeToBlack::Normal) ||
			!applyGain || !driver)
			return;
		const bool black = latest.apl < 17.0 && latest.highlight < 20.0;
		if (!black) blackSinceMs = -1.0;
		else if (blackSinceMs < 0.0) blackSinceMs = timeMs;
		// Light coming in the queue: leave before it reaches the screen.
		bool lightAhead = false;
		if (sequence != 0)
			for (auto it = predictions.upper_bound(sequence);
				it != predictions.end() && it->first <= sequence + lookaheadDepth; ++it)
				lightAhead = lightAhead || Corrected(it->second).apl >= 17.5;
		const DriverStatus status = driver->Status();
		switch (fadeToBlack)
		{
		case FadeToBlack::Normal:
			// Only with look-ahead: leaving must be seen coming.
			if (led.ftbCustomDim != FtbCustomDim::Off && black && !lightAhead && lookaheadCovers &&
				timeMs - blackSinceMs >= led.ftbBlackMs && modelledLevel <= led.ledFloorBlack &&
				!status.busy && !status.sequenceBusy &&
				driver->QueueSequence(CustomDimDown(led), led.ftbStepMs))
			{
				fadeToBlack = FadeToBlack::Deep;
				pendingLog = led.ftbCustomDim == FtbCustomDim::Turns ?
					"LED drive: fade to black - custom dim on, colours taking turns" :
					"LED drive: fade to black - custom dim on, colours together";
				pendingLogSent = true;
			}
			break;
		case FadeToBlack::Deep:
			if ((!black || lightAhead || led.ftbCustomDim == FtbCustomDim::Off) &&
				driver->QueueSequenceNow(CustomDimUp(led), led.ftbStepMs))
			{
				fadeToBlack = FadeToBlack::Leaving;
				// Then the LED straight to the coming scene's level.
				driver->SetLightJump(compensator.Light());
				pendingLog = "LED drive: fade to black - picture returning, custom dim off";
				pendingLogSent = true;
			}
			break;
		case FadeToBlack::Leaving:
			if (!status.sequenceBusy)
			{
				fadeToBlack = FadeToBlack::Normal;
				// The controller may have moved meanwhile: resend its level.
				driver->SetLight(compensator.Light());
			}
			break;
		}
	}

	bool LedDriveModule::CustomDimEngaged() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return fadeToBlack != FadeToBlack::Normal;
	}

	CompensationGain LedDriveModule::GainLocked() const
	{
		CompensationGain current = gain;
		if (compensating && applyGain)
		{
			// Follow the LED's modelled level every frame, never the target, so
			// content brightness stays constant while it moves.
			current.active = true;
			current.domain = GainDomain::Linear;
			current.gain = holding ? heldGain : CompensationGainFor(flux, led.ledCeiling, modelledLevel, policy);
		}
		return current;
	}

	CompensationGain LedDriveModule::CurrentGain() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		CompensationGain current = GainLocked();
		if (!compensating)
		{
			const double now = NowMs();
			for (const auto& scheduled : scheduledGains)
			{
				if (scheduled.first > now) break;
				current = scheduled.second;
			}
		}
		return current;
	}

	bool LedDriveModule::DrivingProjector() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return live;
	}

	DriverStatus LedDriveModule::ProjectorStatus() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return driver ? driver->Status() : DriverStatus{};
	}

	int LedDriveModule::Ticks() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return ticks;
	}

	bool LedDriveModule::WaitUntil(std::chrono::steady_clock::time_point when)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return !wake.wait_until(lock, when, [this] { return stopping; });
	}

	void LedDriveModule::Run()
	{
		const std::string protocol = EffectiveProtocol(projector, led);
		std::unique_ptr<IProjectorDriver> opened = CreateProjectorDriver(protocol, projector, portFactory);
		std::string error;
		bool driving = protocol != "none";
		if (!opened->Open(error))
		{
			DebugLog::Log("LED drive: %s; continuing as a dry run", error.c_str());
			opened = CreateProjectorDriver("none", projector, portFactory);
			opened->Open(error);
			driving = false;
		}
		BringToKnownState(*opened, led.ledCeiling, led.ftbCustomDim != FtbCustomDim::Off);
		const LightRange range = opened->QueryLightRange();
		DebugLog::Log("LED drive: running mode=%s protocol=%s tick=%d ms gamma_boost=%.2f led_latency=%d ms",
			ModeName(led.mode), driving ? protocol.c_str() : "none (dry run)", led.tickMs, led.gammaBoost,
			projector.ledLatencyMs);
		if (flux.Linear())
			DebugLog::Log("LED drive: no flux_table; assuming flux proportional to LPW1 (lpw_max=%d%s)",
				range.known ? range.maximum : led.ledCeiling, range.known ? "" : ", from led_ceiling");
		{
			std::lock_guard<std::mutex> guard(mutex);
			driver = std::move(opened);
			seenSends = driver->Status().lightSends;
			live = driving;
			compensating = led.mode == Mode::Compensate;
			// Compensation is applied to the picture only while the projector
			// is really being driven; a dry run never changes the picture.
			applyGain = driving && led.mode != Mode::LogOnly;
		}

		LedDriveConfig current;
		uint64_t generation = 0;
		{
			std::lock_guard<std::mutex> guard(mutex);
			current = led;
			generation = configGeneration;
		}
		ScriptController controller(current);
		controller.Reset(current.ledCeiling, 50);
		std::string lastLog;
		auto lastLogTime = std::chrono::steady_clock::now();
		for (;;)
		{
			const auto tickStart = std::chrono::steady_clock::now();
			FrameStats stats;
			bool have = false;
			{
				std::lock_guard<std::mutex> guard(mutex);
				if (stopping) break;
				if (generation != configGeneration)
				{
					// New settings: the script controller carries on from the
					// LED's current level (contrast back to unity).
					generation = configGeneration;
					current = led;
					const DriverStatus status = driver->Status();
					controller = ScriptController(current);
					controller.Reset(status.lastSentLight >= 0 ? status.lastSentLight : current.ledCeiling, 50);
				}
				have = haveStats;
				if (!compensating)
				{
					stats = latest;
					stats.cut = pendingCut;
					stats.fade = pendingFade;
					pendingCut = pendingFade = false;
				}
			}
			int durationMs = 0;
			if (compensating)
			{
				// mode: compensate steps per frame in BeginFrame(); this thread
				// only logs, so the render thread never writes the log.
				std::string log;
				bool sent = false;
				{
					std::lock_guard<std::mutex> guard(mutex);
					log.swap(pendingLog);
					sent = pendingLogSent;
					pendingLogSent = false;
				}
				const auto now = std::chrono::steady_clock::now();
				if (!log.empty() && (sent || now - lastLogTime >= std::chrono::seconds(10)))
				{
					DebugLog::Log("%s%s", log.c_str(), applyGain ? "" : " (dry run)");
					lastLogTime = now;
				}
			}
			else if (have)
			{
				const ControllerOutput out = controller.Tick(stats);
				if (out.lightCommand >= 0) driver->SetLight(out.lightCommand);
				for (const ContrastStep& step : out.contrastSchedule)
				{
					if (!WaitUntil(tickStart + std::chrono::milliseconds(step.offsetMs))) break;
					std::lock_guard<std::mutex> guard(mutex);
					// The emulated contrast lands with the LED command the script
					// sent alongside it: led_latency_ms later.
					CompensationGain next = gain;
					next.active = applyGain;
					next.gain = applyGain ? step.contrast / 50.0 : 1.0;
					next.domain = out.gainDomain;
					scheduledGains.push_back({ NowMs() + projector.ledLatencyMs, next });
				}
				durationMs = out.durationMs;
				const auto now = std::chrono::steady_clock::now();
				// Log changes, plus a heartbeat every ten seconds.
				if (out.lightCommand >= 0 || !out.contrastSchedule.empty() ||
					now - lastLogTime >= std::chrono::seconds(10))
				{
					if (out.log != lastLog || now - lastLogTime >= std::chrono::seconds(10))
						DebugLog::Log("%s%s", out.log.c_str(), applyGain ? "" : " (dry run)");
					lastLog = out.log;
					lastLogTime = now;
				}
				std::lock_guard<std::mutex> guard(mutex);
				++ticks;
			}
			if (!WaitUntil(tickStart + std::chrono::milliseconds(std::max(current.tickMs, durationMs))))
				break;
		}

		{
			std::lock_guard<std::mutex> guard(mutex);
			gain.active = false;
			gain.gain = 1.0;
			scheduledGains.clear();
			compensating = false;
		}
		if (driving)
			RestoreFullLight(*driver, current.ledCeiling,
				current.ftbCustomDim != FtbCustomDim::Off || fadeToBlack != FadeToBlack::Normal);
		driver->Close();
	}
}
