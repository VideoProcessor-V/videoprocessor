#include <pch.h>
#include "LedDriveModule.h"

#include <fstream>

#include "../DebugLog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <sstream>

namespace LedDrive
{
	namespace
	{
		const char* ModeName(Mode mode)
		{
			return mode == Mode::Compensate ? "compensate" : mode == Mode::Script ? "script" :
				mode == Mode::PictureOnly ? "picture_only" : "log_only";
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
		ConfigureModelsLocked();
	}

	void LedDriveModule::ConfigureModelsLocked()
	{
		ledModel.SetSettle(projector.ledSettleMs);
		contrastModel.SetSettle(projector.cntrSettleMs);
		// The assist's ceiling: cntr_assist_max (62 until 7c-2's dump, the
		// planner's 7c-1 answer 3), never above cntr_max_usable.
		const int assistMax = std::max(50, std::min(led.cntrAssistMax, projector.cntrMaxUsable));
		pairing.Configure(led.compensationEotf, assistMax, led.cntrAssist, led.softwareFirst);
		// The highlight floor may use CNTR's headroom only up to where white
		// still fits on the DMD (dmd_clip_cntr; 7c-2).
		compensator.SetContrastHeadroom(led.cntrAssist ?
			std::pow(std::min(assistMax, DmdClipLocked()) / 50.0, led.compensationEotf) : 1.0);
		recovery.Configure(led, flux);
	}

	std::vector<std::pair<double, double>> LedDriveModule::LedLandingsLocked(int pendingLevel) const
	{
		std::vector<std::pair<double, double>> landings;
		const int latency = LatencyFramesLocked();
		const double period = std::max(1.0, framePeriodMs);
		for (const Command& command : commands)
		{
			const double atMs = command.landMs >= 0.0 && lastFrameMs >= 0.0 ? command.landMs - lastFrameMs :
				(static_cast<double>(command.frame + latency) - static_cast<double>(frameIndex)) * period;
			landings.push_back({ std::max(0.0, atMs), LedFractionLocked(command.level) });
		}
		// This tick's LPW1, not yet written: lands led_latency_ms after it goes out.
		if (pendingLevel >= 0)
			landings.push_back({ static_cast<double>(projector.ledLatencyMs), LedFractionLocked(pendingLevel) });
		std::stable_sort(landings.begin(), landings.end(),
			[](const std::pair<double, double>& a, const std::pair<double, double>& b) { return a.first < b.first; });
		return landings;
	}

	int LedDriveModule::CutLevelLocked() const
	{
		// audit 2026-10-16: never above the assist's ceiling (cntr_assist_max,
		// itself within cntr_max_usable), as for every other CNTR command.
		const int assistMax = std::max(50, std::min(led.cntrAssistMax, projector.cntrMaxUsable));
		return std::min(led.cutCntrLevel >= 50 ? led.cutCntrLevel : DmdClipLocked(), assistMax);
	}

	int LedDriveModule::DmdClipLocked() const
	{
		return projector.dmdClipCntr >= 50 ? projector.dmdClipCntr : std::max(50, projector.cntrMaxUsable);
	}

	bool LedDriveModule::Recovering() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return recovery.Active() || pendingRecovery;
	}

	double LedDriveModule::RecoveryDisplayed() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return recovery.Active() ? recovery.Displayed() : 1.0;
	}

	double LedDriveModule::LedFractionLocked(double level) const
	{
		const double ceiling = flux.Flux(led.ledCeiling);
		return ceiling > 0.0 ? flux.Flux(level) / ceiling : 1.0;
	}

	// Drops a CNTR still waiting for the port and puts the pairing back on
	// the last value the driver really sent, so model and projector agree.
	void LedDriveModule::HoldContrastLocked()
	{
		if (!driver) return;
		driver->SetContrastAsync(-1);
		const DriverStatus status = driver->Status();
		pairing.Reset(status.lastSentContrast >= 0 ? status.lastSentContrast : 50);
	}

	int LedDriveModule::LatencyFramesForLocked(int ms) const
	{
		return static_cast<int>(std::lround(ms / framePeriodMs));
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
		const bool wasScript = led.mode == Mode::Script;
		projector = projectorIn;
		led = ledIn;
		flux = FluxModel(projector.fluxTable);
		statsBuilder = StatsBuilder(led);
		compensator.Reconfigure(led, flux);
		ConfigureModelsLocked();
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
				// The models start from the LED's level and CNTR 50 (script
				// mode puts CNTR back to 50 as it leaves).
				modelledLevel = status.lastSentLight >= 0 ? status.lastSentLight : modelledLevel;
				commands.clear();
				ledModel.Reset(LedFractionLocked(modelledLevel));
				contrastCommands.clear();
				seenContrastSends = status.contrastSends;
				pairing.Reset(50);
				pairing.SetCeiling(-1);
				pairing.SetBias(0.0);
				contrastModel.Reset(1.0);
				recovery.Stop();
				pendingRecovery = false;
				recoveryLimit = 1e9;
				smoothing = false;
				policy = ControllerOutput();
				holding = false;
				// From script: compensate starts only once the script thread
				// has put CNTR back to 50 (it wakes for the new settings).
				// Meanwhile the picture is untouched: no smoothing gain either.
				awaitingContrastReset = wasScript;
				gain.active = false;
				gain.gain = 1.0;
			}
			if (!nowCompensating && wasCompensating)
			{
				gain.active = false;
				gain.gain = 1.0;
				holding = false;
				// A CNTR still waiting for the port is compensate's; the
				// script thread puts CNTR back to 50 (7c-1).
				driver->SetContrastAsync(-1);
				// Bug hunt 2026-10-17: and compensate's LPW1 queue (the rest of a
				// split dimming drop kept landing under the script, which starts
				// from the last level written).
				driver->CancelLight();
			}
			compensating = nowCompensating;
			// picture_only (delta 12): the gain stays exactly 1.
			applyGain = live && led.mode != Mode::LogOnly && led.mode != Mode::PictureOnly;
			// The smoothing gain ends with the settings that wanted it, not at
			// the worker's next pass (audit 2026-10-14b: picture_only must be
			// exactly 1 from the switch).
			const bool smoothNow = led.mode == Mode::Script && led.scriptSmoothing &&
				applyGain && led.adaptiveGamma;
			if (smoothing && !smoothNow)
			{
				smoothing = false;
				if (!compensating)
				{
					gain.active = false;
					gain.gain = 1.0;
				}
			}
			// Only the compensate step leaves the custom dim, so a switch away
			// from it (or the experiment switched off) leaves here: LDMM 0 jumps
			// the driver's queue, ahead of any LPW1 the new mode sends.
			if (fadeToBlack != FadeToBlack::Normal &&
				(!compensating || !applyGain || led.ftbCustomDim == FtbCustomDim::Off))
			{
				if (fadeToBlack == FadeToBlack::Deep)
					driver->QueueSequenceNow(CustomDimUp(led), led.ftbStepMs);
				fadeToBlack = FadeToBlack::Normal;
				blackSinceMs = -1.0;
				DebugLog::Log("LED drive: fade to black - custom dim off (settings changed)");
			}
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
		// Settled: exactly the landed level (no round trip through the flux).
		if (ledModel.Value() == ledModel.Target()) return modelledLevel;
		return flux.LevelOf(ledModel.Value() * flux.Flux(led.ledCeiling));
	}

	int LedDriveModule::AssistContrast() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return pairing.Contrast();
	}

	double LedDriveModule::ModelledContrastLight() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return contrastModel.Value();
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

	size_t LedDriveModule::LedLookaheadFramesLocked() const
	{
		if (led.ledLookaheadMs <= 0) return 0;
		const double periodMs = sourceFps > 0.0 ? 1000.0 / sourceFps : framePeriodMs;
		return static_cast<size_t>(std::lround(led.ledLookaheadMs / std::max(1.0, periodMs)));
	}

	size_t LedDriveModule::WantedLookaheadFrames() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return led.mode == Mode::Compensate && led.lookahead ? LedLookaheadFramesLocked() : 0;
	}

	// led_landing_ms (review 2026-10-17): the write time onto the frame clock.
	// The send came after the previous frame (its status read saw nothing
	// newer), so it is placed that long after the previous frame's time,
	// within the frame interval; with the module's own clock (BeginFrame)
	// this is exactly the write time.
	double LedDriveModule::LandingMsLocked(std::chrono::steady_clock::time_point sentAt, int latencyMs,
		double timeMs, double previousFrameMs, double previousWallMs) const
	{
		if (!led.ledLandingMs || sentAt == std::chrono::steady_clock::time_point()) return -1.0;
		const double sentWallMs = std::chrono::duration<double, std::milli>(sentAt - created).count();
		// Render-ahead (BeginFrameAhead): a frame's time is its display time, aheadMs
		// after its wall time; a send lands by the wall clock, so each frame's time is
		// taken back to its wall time here (both 0 otherwise: unchanged).
		const double previousMs = previousFrameMs - previousFrameAheadMs;
		const double currentMs = timeMs - frameAheadMs;
		double sentMs = currentMs;
		if (previousFrameMs >= 0.0 && previousWallMs >= 0.0)
			sentMs = previousMs + std::max(0.0, std::min(sentWallMs - previousWallMs, currentMs - previousMs));
		else
		{
			// Integration 2026-10-17: the first frame. Its wall time (lastFrameWallMs, set by
			// BeginFrameAt just before) anchors timeMs; with a test or replay clock timeMs is
			// not the wall clock, so "now" must not be assumed to be timeMs.
			const double frameWallMs = lastFrameWallMs >= 0.0 ? lastFrameWallMs : NowMs();
			sentMs = currentMs - std::max(0.0, frameWallMs - sentWallMs);
		}
		return sentMs + latencyMs;
	}

	// A send lands on the frame nearest its landing time (by milliseconds), or
	// latency whole frames after the frame it was written during (by frames).
	bool LedDriveModule::LandsLocked(uint64_t frame, double landMs, int latencyFrames, double timeMs) const
	{
		if (landMs >= 0.0) return landMs <= timeMs + framePeriodMs * 0.5;
		return frame + latencyFrames <= frameIndex;
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
			// audit 2026-10-16: weighted by the time since the last update (it
			// runs once per new source frame, not per output frame): the output
			// frames rendered since, at least one, at most 250 ms. Behind
			// correction_weight_by_time (default on); off = one output frame
			// period per update, as before.
			const uint64_t frames = std::max<uint64_t>(1, frameIndex - std::min(frameIndex, lastCorrectionFrame));
			const double elapsedMs = led.correctionWeightByTime ?
				std::min(static_cast<double>(frames) * framePeriodMs, 250.0) : framePeriodMs;
			lastCorrectionFrame = frameIndex;
			const double weight = std::min(1.0, elapsedMs / CORRECTION_WINDOW_MS);
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
		lastWalkWallMs = NowMs();   // review 2026-10-17
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

	CompensationGain LedDriveModule::BeginFrameAhead(uint64_t sequence, double aheadMs, double sourceFpsIn)
	{
		const double ahead = aheadMs > 0.0 && aheadMs < 1000.0 ? aheadMs : 0.0;
		{
			std::lock_guard<std::mutex> guard(mutex);
			nextFrameAheadMs = ahead;
		}
		return BeginFrameAt(sequence, NowMs() + ahead, sourceFpsIn);
	}

	CompensationGain LedDriveModule::BeginFrameAt(uint64_t sequence, double timeMs, double sourceFpsIn)
	{
		std::lock_guard<std::mutex> guard(mutex);
		// Render-ahead: the previous and this frame's lead over the wall clock (0 for
		// BeginFrame / a test clock), so that a send's landing stays on the wall clock.
		previousFrameAheadMs = frameAheadMs;
		frameAheadMs = nextFrameAheadMs;
		nextFrameAheadMs = 0.0;
		sourceFps = sourceFpsIn > 0.0 && sourceFpsIn < 1000.0 ? sourceFpsIn : 0.0;
		double elapsed = framePeriodMs;
		const double previousFrameMs = lastFrameMs, previousWallMs = lastFrameWallMs;   // led_landing_ms
		if (lastFrameMs >= 0.0)
		{
			elapsed = std::max(1.0, std::min(timeMs - lastFrameMs, 250.0));
			framePeriodMs = std::max(1.0, std::min(framePeriodMs + (elapsed - framePeriodMs) * 0.1, 100.0));   // 10 to 1000 frames per second
		}
		lastFrameMs = timeMs;
		lastFrameWallMs = NowMs();   // audit 2026-10-16
		idleRestored = false;
		// Levels the driver wrote since the last frame: they went out during
		// that frame.
		if (compensating && applyGain && driver)
		{
			const DriverStatus status = driver->Status();
			// led_landing_ms (review 2026-10-17): each lands its latency after
			// the driver wrote it, in ms; off = by frames from this frame.
			if (status.lightSends != seenSends && status.lastSentLight >= 0)
				commands.push_back({ frameIndex, status.lastSentLight, LandingMsLocked(status.lastLightSentAt,
					projector.ledLatencyMs, timeMs, previousFrameMs, previousWallMs) });
			seenSends = status.lightSends;
			if (status.contrastSends != seenContrastSends && status.lastSentContrast >= 0)
				contrastCommands.push_back({ frameIndex, status.lastSentContrast, LandingMsLocked(
					status.lastContrastSentAt, projector.cntrLatencyMs, timeMs, previousFrameMs, previousWallMs) });
			seenContrastSends = status.contrastSends;
			// audit 2026-10-16: with no CNTR waiting or in flight, the pairing
			// believes what the projector was last sent. A CNTR the driver was
			// already writing when a hold dropped it, or one whose write failed,
			// would otherwise leave them apart: the pairing sends only when its
			// value changes, so CNTR could stay above the need (a stuck 51).
			// Not while a pre-position hold keeps CNTR up.
			if (!status.contrastBusy && status.lastSentContrast >= 0 && pairing.Hold() < 0 &&
				pairing.Contrast() != status.lastSentContrast)
				pairing.Reset(status.lastSentContrast);
		}
		++frameIndex;
		if (smoothing && !compensating)
		{
			// mode: script with script_smoothing: the bridge across the
			// projector's CNTR steps, on the encoded signal like CNTR itself.
			gain.active = true;
			gain.domain = GainDomain::Encoded;
			gain.gain = smoother.Gain(timeMs);
		}
		if (compensating && haveStats && !awaitingContrastReset)
		{
			StepCompensateLocked(sequence, elapsed);
			StepFadeToBlackLocked(sequence, timeMs);
		}
		// Dead time: the LED reaches a commanded level led_latency_ms (in
		// whole frames) after the command, and the gain switches with it.
		const int latency = LatencyFramesLocked();
		while (!commands.empty() && LandsLocked(commands.front().frame, commands.front().landMs, latency, timeMs))
		{
			modelledLevel = commands.front().level;
			commands.pop_front();
			ledModel.Land(LedFractionLocked(modelledLevel));
		}
		// 7c-1: then the LED's light settles (led_settle_ms; 0 = at once).
		ledModel.Step(elapsed);
		const int contrastLatency = LatencyFramesForLocked(projector.cntrLatencyMs);
		while (!contrastCommands.empty() &&
			LandsLocked(contrastCommands.front().frame, contrastCommands.front().landMs, contrastLatency, timeMs))
		{
			contrastModel.Land(pairing.Light(contrastCommands.front().value));
			contrastCommands.pop_front();
		}
		contrastModel.Step(elapsed);
		// 7c-2: the fade-up starts on the cut frame itself (a seen cut is
		// known latency frames ahead) and limits VP's gain while it runs.
		if (pendingRecovery && compensating &&
			(pendingRecoverySequence == 0 || sequence == 0 || sequence >= pendingRecoverySequence))
		{
			pendingRecovery = false;
			// From the CNTR just sent, not the modelled one: before the dump
			// lands the limit simply trims the extra, so nothing steps down.
			recovery.Start(ledModel.Value(), pairing.Light(pairing.Contrast()), pendingRecoveryCeiling);
		}
		recoveryLimit = compensating ? recovery.Step(elapsed, ledModel.Value(), contrastModel.Value()) : 1e9;
		if (!recovery.Active() && !pendingRecovery && pairing.Ceiling() >= 0)
		{
			// The fade-up is over: CNTR pairs normally again.
			pairing.SetCeiling(-1);
			pairing.SetBias(0.0);
		}
		if (holding && !policy.holdGain) holding = false;
		if (policy.holdGain && !holding)
		{
			heldGain = CompensationGainForLed(1.0 / std::max(1e-6, ledModel.Value()), policy, contrastModel.Value());
			holding = true;
		}
		while (!predictions.empty() && predictions.begin()->first + PREDICTION_HISTORY < sequence)
			predictions.erase(predictions.begin());
		// Delta 11: the veil estimate from the latest statistics.
		if (haveStats)
		{
			// The frame's own highlight percentile (the slew below keeps a
			// flash from pumping it).
			const double level = std::max(0.0, std::min((latest.highlight - 16.0) / 219.0, 1.0));
			const double statistic = level * latest.highlightArea;
			const double lo = led.shadowLiftAnsiRampLo, hi = std::max(lo + 1e-6, led.shadowLiftAnsiRampHi);
			const double t = std::max(0.0, std::min((statistic - lo) / (hi - lo), 1.0));
			const double target = t * t * (3.0 - 2.0 * t);
			if (latest.cut || !veilValid)
				gain.veil = target;
			else
			{
				const double step = std::max(0.1, std::min(led.shadowLiftAnsiRate, 10.0)) * elapsed / 1000.0;
				gain.veil += std::max(-step, std::min(target - gain.veil, step));
			}
			veilValid = true;
		}
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
		// lookahead off (owner 2026-10-14): the latest exact statistics only.
		const bool predicted = led.lookahead && sequence != 0 && first != forCommand;
		// The look-ahead reaches the frame the LED will land on.
		const bool covered = predicted && std::prev(forCommand)->first >= sequence + static_cast<uint64_t>(latency);
		uint64_t cutSequence = 0;
		if (predicted)
		{
			target = Corrected(std::prev(forCommand)->second);
			for (auto it = first; it != forCommand; ++it)
			{
				if (it->second.cut && cutSequence == 0) cutSequence = it->first;
				target.cut = target.cut || it->second.cut;
			}
		}
		pendingCut = pendingFade = false;
		// The furthest queued frame, for early ramps.
		LedDrive::Lookahead ahead;
		bool haveAhead = false;
		// lookahead off (owner 2026-10-14): reactive, as with no queue.
		size_t depth = led.lookahead ? lookaheadDepth : 0;
		// led_lookahead_ms (review 2026-10-17): at most that far, when set.
		if (led.ledLookaheadMs > 0) depth = std::min(depth, LedLookaheadFramesLocked());
		// Review 2026-10-17: a clear warning (once per change) when the
		// look-ahead is wanted but has no depth - active_picture_lookahead_frames
		// 0 (the shipped sample, Low_Latency) or no walk for a second - so
		// every cut is reactive.
		{
			const bool walkStale = lastWalkWallMs < 0.0 || NowMs() - lastWalkWallMs > 1000.0;
			const int zero = led.lookahead && (lookaheadDepth == 0 || walkStale) ? 1 : 0;
			if (zero != lookaheadWarnState)
			{
				if (zero)
					pendingWarning = "LED drive: WARNING - compensate has lookahead on but a look-ahead depth of 0 "
						"(the queue profile's active_picture_lookahead_frames is 0, or no queued-frame walk): "
						"every cut and exit from black is reactive";
				else if (lookaheadWarnState == 1)
					pendingWarning = "LED drive: look-ahead depth available again (" + std::to_string(lookaheadDepth) +
						" frames)";
				lookaheadWarnState = zero;
			}
		}
		const auto horizon = predictions.upper_bound(sequence + depth);
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
		// CNTR assist (7c-1): CNTR follows the LED's modelled light for what
		// VP's gain cannot do within the highlight tolerance, as a command
		// only when the paired whole value changes, on the modelled light now
		// (never ahead of the LED; policy 3.4). Not during a fade's gain hold
		// or the custom dim.
		int contrastCommand = -1;
		const bool allowed = applyGain && fadeToBlack == FadeToBlack::Normal && !out.holdGain;
		// A fade's gain hold (or the custom dim) begins: a CNTR still waiting
		// for the port must not land during it (audit stage7-step7c1).
		if (assistAllowed && !allowed) HoldContrastLocked();
		assistAllowed = allowed;
		// 7c-2, bright cut (control policy 3.3 as corrected by the owner,
		// 2026-10-11): once per cut, CNTR goes to cut_cntr_level (59 by
		// default; higher only to keep the fade-up's start at max_cut_dip;
		// never above where it is), never to 50 because of the cut: VP's gain
		// cannot add absolute light. Sent now so it lands with the LED's jump
		// on the cut frame (CNTR takes the slot ahead of the LPW1). During the
		// fade-up CNTR never rises; it descends paired as the LED rises, with
		// recovery_bias_pct.
		if (out.brightCut && (cutSequence == 0 || cutSequence != lastBrightCutSequence))
		{
			lastBrightCutSequence = cutSequence;
			const double dmd = pairing.Light(DmdClipLocked());
			const int cutLevel = CutLevelLocked();
			if (allowed && led.cutCntrDump && pairing.Contrast() > cutLevel)
			{
				const int dump = pairing.DumpTarget(ledModel.Value(), out.cutCeiling, led.maxCutDip, cutLevel);
				if (dump < pairing.Contrast())
				{
					pairing.Reset(dump);
					driver->SetContrastAsync(dump);
					contrastCommand = dump;
				}
			}
			pairing.SetCeiling(pairing.Contrast());
			pairing.SetBias(led.recoveryBiasPct / 100.0);
			if (led.shapedRecovery)
			{
				pendingRecovery = true;
				pendingRecoverySequence = cutSequence;
				// The new scene's headroom (the knee's tolerance), shrunk in
				// proportion when CNTR is above where white clips on the DMD.
				pendingRecoveryCeiling = out.cutCeiling *
					std::min(1.0, dmd / pairing.Light(pairing.Contrast()));
			}
		}
		else if (out.darkCut)
		{
			// Policy 3.4: no fade-up; CNTR pairs normally.
			recovery.Stop();
			pendingRecovery = false;
			pairing.SetCeiling(-1);
			pairing.SetBias(0.0);
		}
		// 7c-3, exit from black: while the picture is black or very dark and a
		// bright scene is coming, CNTR goes to cut_cntr_level and is held
		// there until the cut (invisible on black; VP's gain divides by the
		// modelled CNTR light anyway). Also while the custom dim is leaving.
		if (out.preposition && led.cntrAssist && applyGain && fadeToBlack != FadeToBlack::Deep && !out.holdGain)
		{
			int level = CutLevelLocked();
			// Post-merge audit 2026-10-17: the cut_lead window's limit (CNTR does not
			// rise while a cut is pending) caps the pre-position too.
			if (out.cutLeadHold) level = std::min(level, pairing.Contrast());
			pairing.SetHold(level);
			if (contrastCommand < 0 && pairing.Contrast() < level)
			{
				pairing.Reset(level);
				driver->SetContrastAsync(level);
				contrastCommand = level;
			}
		}
		else
			pairing.SetHold(-1);
		// The LED command, after a cut's or a pre-position's CNTR (audit
		// stage7-step7c, Should fix 1): queued first, the CNTR takes the next
		// slot and the LPW1 follows a command gap later, so the CNTR lands with
		// the cut. Queued to the driver's thread, which keeps only the newest
		// and sends it when the command gap allows; the model follows what it
		// actually sends. Within a scene the pairing's CNTR (below) comes
		// after the LPW1 (dark direction: LPW1 before CNTR).
		if (out.lightCommand >= 0 && applyGain && fadeToBlack != FadeToBlack::Deep)
		{
			if (out.jump) driver->SetLightJump(out.lightCommand);
			else driver->SetLight(out.lightCommand);
		}
		if (allowed && contrastCommand < 0)
		{
			// CNTR is paired with the LED's modelled light now: never above the
			// paired value for the modelled LED (policy 3.4; planner's 7c-2
			// answer 4). While the scene has highlights (VP's headroom is
			// limited), CNTR stops at dmd_clip_cntr: above it more CNTR adds no
			// highlight light (the highlight floor keeps the LED where that
			// suffices). Feeding the clip into the headroom instead would make
			// each CNTR step ask for the next.
			int limit = out.vpHeadroom < 1e8 ? DmdClipLocked() : -1;
			// cut_lead: window (review 2026-10-17): while a led cut is pending
			// CNTR does not rise (the port is kept for the cut); it may still
			// descend, never staying above the need.
			if (out.cutLeadHold) limit = limit >= 50 ? std::min(limit, pairing.Contrast()) : pairing.Contrast();
			// cntr_pair_predict (post-merge audit 2026-10-17, owner rule): paired against
			// the LED where it will be when this CNTR lands (cntr_latency_ms ahead), so
			// CNTR is never above the need at landing. Off: the LED now, as before.
			const double pairedLed = CntrPairLedFraction(led.cntrPairPredict, ledModel,
				led.cntrPairPredict ? LedLandingsLocked(out.lightCommand >= 0 && applyGain &&
					fadeToBlack != FadeToBlack::Deep ? out.lightCommand : -1) : std::vector<std::pair<double, double>>(),
				projector.cntrLatencyMs);
			contrastCommand = pairing.Update(1.0 / std::max(1e-6, pairedLed), out.vpHeadroom, limit);
			if (contrastCommand >= 0) driver->SetContrastAsync(contrastCommand);
		}
		if (contrastCommand > 50) assistContrastDriven = true;
		policy = out;
		gain.knee = out.knee;
		++ticks;
		if (out.lightCommand >= 0 || contrastCommand >= 0 || pendingLog.empty())
		{
			// The LED : CNTR command ratio for the owner (planner's step 7
			// answer 1).
			const DriverStatus status = driver->Status();
			std::ostringstream log;
			log << out.log << " cntr=" << pairing.Contrast() << (contrastCommand >= 0 ? " sent" : "")
				<< " lpw1:cntr=" << status.lightSends << ":" << status.contrastSends;
			pendingLog = log.str();
			pendingLogSent = out.lightCommand >= 0 || contrastCommand >= 0;
		}
	}

	void LedDriveModule::StepFadeToBlackLocked(uint64_t sequence, double timeMs)
	{
		if ((led.ftbCustomDim == FtbCustomDim::Off && fadeToBlack == FadeToBlack::Normal) ||
			!applyGain || !driver)
			return;
		const bool black = IsTrueBlack(latest);
		if (!black) blackSinceMs = -1.0;
		else if (blackSinceMs < 0.0) blackSinceMs = timeMs;
		// Light coming in the queue: leave before it reaches the screen.
		bool lightAhead = false;
		if (sequence != 0)
			for (auto it = predictions.upper_bound(sequence);
				it != predictions.end() && it->first <= sequence + (led.lookahead ? lookaheadDepth : 0); ++it)
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
				HoldContrastLocked();
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

	double LedDriveModule::MsSinceLastFrame() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return MsSinceLastFrameLocked();
	}

	double LedDriveModule::MsSinceLastFrameLocked() const
	{
		// audit 2026-10-16: on the module's own clock when the frame arrived
		// (BeginFrameAt's time may be another clock, as in the tests).
		return lastFrameWallMs < 0.0 ? 1e12 : NowMs() - lastFrameWallMs;
	}

	// audit 2026-10-16: led_idle_restore_s in compensate - no video for that
	// long (pause, stop): out of the custom dim (LDMM 0, when configured or
	// engaged), CNTR to 50 when it is or may be elsewhere, the LED to the
	// ceiling; the controller and models start again from there, waiting for
	// a fresh measurement.
	void LedDriveModule::RestoreIdleCompensateLocked()
	{
		if (!driver) return;
		const DriverStatus status = driver->Status();
		if (fadeToBlack != FadeToBlack::Normal || led.ftbCustomDim != FtbCustomDim::Off)
			driver->QueueSequenceNow(CustomDimUp(led), led.ftbStepMs);
		fadeToBlack = FadeToBlack::Normal;
		blackSinceMs = -1.0;
		if (pairing.Contrast() != 50 || status.contrastBusy ||
			(status.lastSentContrast >= 0 && status.lastSentContrast != 50))
			driver->SetContrastAsync(50);
		driver->SetLightJump(led.ledCeiling);
		compensator.Reset(led.ledCeiling);
		pairing.Reset(50);
		pairing.SetCeiling(-1);
		pairing.SetBias(0.0);
		pairing.SetHold(-1);
		recovery.Stop();
		pendingRecovery = false;
		recoveryLimit = 1e9;
		policy = ControllerOutput();
		holding = false;
		modelledLevel = led.ledCeiling;
		commands.clear();
		ledModel.Reset(LedFractionLocked(modelledLevel));
		contrastCommands.clear();
		contrastModel.Reset(1.0);
		haveStats = false;
		pendingCut = pendingFade = false;
	}

	bool LedDriveModule::CustomDimEngaged() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return fadeToBlack != FadeToBlack::Normal;
	}

	CompensationGain LedDriveModule::GainLocked() const
	{
		CompensationGain current = gain;
		// adaptive_gamma off (owner 2026-10-14): compensate still drives the
		// LED and CNTR; only the picture's gain stays at 1 (audit 2026-10-14b).
		if (compensating && applyGain && led.adaptiveGamma && !awaitingContrastReset)
		{
			// Follow the LED's modelled level every frame, never the target, so
			// content brightness stays constant while it moves.
			current.active = true;
			current.domain = GainDomain::Linear;
			current.gain = holding ? heldGain : std::min(recoveryLimit,
				CompensationGainForLed(1.0 / std::max(1e-6, ledModel.Value()), policy, contrastModel.Value()));
		}
		return current;
	}

	CompensationGain LedDriveModule::CurrentGain() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return GainLocked();
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

	bool LedDriveModule::WaitUntil(std::chrono::steady_clock::time_point when, uint64_t generation)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return !wake.wait_until(lock, when, [this, generation] { return stopping || configGeneration != generation; });
	}

	void LedDriveModule::Run()
	{
		// Bug hunt 2026-10-17: the start-up below (open, handshake, light
		// range) can take seconds; Reconfigure reassigns the members under the
		// mutex meanwhile, so the start-up works on copies taken under it.
		// Review 2026-10-17: named apart from the members, which the loop
		// below keeps reading under the mutex (a shadowing copy there would
		// reload the start-up settings after every Reconfigure).
		ProjectorConfig startProjector;
		LedDriveConfig startLed;
		{
			std::lock_guard<std::mutex> guard(mutex);
			startProjector = projector;
			startLed = led;
		}
		const FluxModel startFlux(startProjector.fluxTable);
		const std::string protocol = EffectiveProtocol(startProjector, startLed);
		// Retired source (owner 2026-10-15): an older file naming it runs
		// VP's own copy of the script.
		if (startLed.pythonSourceRetired)
			DebugLog::Log("LED drive: command_source: python / mode: listen is retired; this configuration runs VP's "
				"copy of the script (internal_script), which drives the port itself");
		std::unique_ptr<IProjectorDriver> opened = CreateProjectorDriver(protocol, startProjector, portFactory);
		std::string error;
		bool driving = protocol != "none";
		if (!opened->Open(error))
		{
			DebugLog::Log("LED drive: %s; continuing as a dry run", error.c_str());
			opened = CreateProjectorDriver("none", startProjector, portFactory);
			opened->Open(error);
			driving = false;
		}
		// audit 2026-10-16: LDMM 0 at every start on a driven projector (one
		// command), not only while the custom dim is configured: a crash during
		// the custom dim with the setting later switched off left the
		// projector in custom mode.
		BringToKnownState(*opened, startLed.ledCeiling, driving || startLed.ftbCustomDim != FtbCustomDim::Off,
			startLed.mode == Mode::Script);
		const LightRange range = opened->QueryLightRange();
		DebugLog::Log("LED drive: running mode=%s protocol=%s tick=%d ms gamma_boost=%.2f led_latency=%d ms",
			ModeName(startLed.mode), driving ? protocol.c_str() : "none (dry run)", startLed.tickMs, startLed.gammaBoost,
			startProjector.ledLatencyMs);
		if (startFlux.Linear())
			DebugLog::Log("LED drive: no flux_table; assuming flux proportional to LPW1 (lpw_max=%d%s)",
				range.known ? range.maximum : startLed.ledCeiling, range.known ? "" : ", from led_ceiling");
		{
			std::lock_guard<std::mutex> guard(mutex);
			driver = std::move(opened);
			seenSends = driver->Status().lightSends;
			live = driving;
			// The settings now in force (a Reconfigure during the start-up
			// found no driver and only stored them).
			compensating = led.mode == Mode::Compensate;
			// Compensation is applied to the picture only while the projector
			// is really being driven; a dry run never changes the picture.
			applyGain = driving && led.mode != Mode::LogOnly && led.mode != Mode::PictureOnly;
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
		// Set once script mode has sent a CNTR other than 50: the exit, or a
		// switch to compensate, puts it back to 50.
		bool contrastDriven = false;
		bool restoreAssist = false;
		// Bug hunt 2026-10-17: a script LPW1/CNTR whose write failed is sent
		// again at the next tick (the controller already believes it, so the
		// script itself would never repeat it: CNTR stuck at 51).
		int resendLight = -1;
		int resendContrast = -1;
		// Post-merge audit 2026-10-17: a script send's resend value - only when the
		// write itself did not happen. A missing reply is not a failed write (the
		// projector may well have taken it; the driver keeps Barco's 2 s after it).
		auto scriptSend = [this](const char* mnemonic, int value) -> int
		{
			bool written = false;
			driver->SendScript(mnemonic, value, &written);
			return written ? -1 : value;
		};
		// The resend log: at most once per 10 s, with the count since the last line.
		std::chrono::steady_clock::time_point resendLogTime{};
		int resendsNotLogged = 0;
		auto logResend = [&](const char* mnemonic, int value)
		{
			++resendsNotLogged;
			const auto now = std::chrono::steady_clock::now();
			if (resendLogTime == std::chrono::steady_clock::time_point{} ||
				now - resendLogTime >= std::chrono::seconds(10))
			{
				DebugLog::Log("LED drive: re-sending %s %d (the last write failed; %d re-send(s) since the last report)",
					mnemonic, value, resendsNotLogged);
				resendLogTime = now;
				resendsNotLogged = 0;
			}
		};
		auto setSmoothing = [this](bool on, int contrast)
		{
			std::lock_guard<std::mutex> guard(mutex);
			if (on != smoothing) smoother.Reset(contrast);
			smoothing = on;
			if (!on && !compensating)
			{
				gain.active = false;
				gain.gain = 1.0;
			}
		};
		auto smoothingWanted = [&] {
			return current.mode == Mode::Script && current.scriptSmoothing && applyGain &&
				current.adaptiveGamma;
		};
		setSmoothing(smoothingWanted(), 50);
		std::string lastLog;
		auto lastLogTime = std::chrono::steady_clock::now();
		for (;;)
		{
			const auto tickStart = std::chrono::steady_clock::now();
			FrameStats stats;
			bool have = false;
			bool restoreScriptIdle = false;   // audit 2026-10-16
			{
				std::lock_guard<std::mutex> guard(mutex);
				if (stopping) break;
				if (generation != configGeneration)
				{
					// New settings: the script controller carries on from the
					// LED's current level and the projector's current CNTR
					// (step 7b: CNTR is real, so it is not reset to 50 here).
					generation = configGeneration;
					current = led;
					const DriverStatus status = driver->Status();
					const int contrastNow = controller.Contrast();
					controller = ScriptController(current);
					controller.Reset(status.lastSentLight >= 0 ? status.lastSentLight : current.ledCeiling,
						contrastNow);
					// From compensate with CNTR assist above 50: back to 50
					// first (7c-1), which the script controller assumes.
					if (current.mode != Mode::Compensate && assistContrastDriven)
					{
						restoreAssist = true;
						assistContrastDriven = false;
						pairing.Reset(50);
						contrastModel.Reset(1.0);
						controller.Reset(controller.Light(), 50);
					}
				}
				// audit 2026-10-16: led_idle_restore_s (0 = off) - no video for
				// that long after frames had come (pause, stop): the projector
				// goes back to full light once, and internal_script no longer
				// acts on the last frame's statistics until frames return.
				const bool idle = driving && current.idleRestoreS > 0 && lastFrameWallMs >= 0.0 &&
					MsSinceLastFrameLocked() >= current.idleRestoreS * 1000.0;
				if (idle && !idleRestored)
				{
					idleRestored = true;
					if (compensating && applyGain)
					{
						RestoreIdleCompensateLocked();
						DebugLog::Log("LED drive: no video for %d s; LED back to %d, CNTR 50 (led_idle_restore_s)",
							current.idleRestoreS, led.ledCeiling);
					}
					else if (current.mode == Mode::Script && applyGain)
						restoreScriptIdle = true;
				}
				have = haveStats && !idle;
				if (!compensating)
				{
					stats = latest;
					stats.cut = pendingCut;
					stats.fade = pendingFade;
					pendingCut = pendingFade = false;
				}
			}
			auto sendsDone = tickStart;
			// The script's sleep after an LPW1 when no CNTR follows it.
			auto scriptFloor = tickStart;
			if (restoreAssist)
			{
				restoreAssist = false;
				const bool ok = driver->SendScript("CNTR", 50);
				DebugLog::Log("LED drive: left compensate; contrast set to 50%s", ok ? "" : " (not confirmed)");
			}
			if (restoreScriptIdle)
			{
				// audit 2026-10-16: led_idle_restore_s in internal_script - full
				// light and CNTR 50 (only what differs from what the script
				// believes), and the script carries on from there when video
				// returns.
				// Bug hunt 2026-10-17: decided from what the port last saw too,
				// not only the controller's belief (a failed write of 50 left
				// the projector at 51 while the script believed 50).
				const DriverStatus status = driver->Status();
				const bool lightAt = controller.Light() == current.ledCeiling && !status.busy &&
					(status.lastSentLight < 0 || status.lastSentLight == current.ledCeiling) && resendLight < 0;
				const bool contrastAt = controller.Contrast() == 50 && !status.contrastBusy &&
					(status.lastSentContrast < 0 || status.lastSentContrast == 50) && resendContrast < 0;
				const bool light = lightAt || driver->SendScript("LPW1", current.ledCeiling);
				const bool contrast = contrastAt || driver->SendScript("CNTR", 50);
				resendLight = resendContrast = -1;
				controller.Reset(current.ledCeiling, 50);
				{
					std::lock_guard<std::mutex> guard(mutex);
					if (smoothing) smoother.Reset(50);
				}
				sendsDone = std::chrono::steady_clock::now();
				DebugLog::Log("LED drive: no video for %d s; LED back to %d%s, CNTR 50%s (led_idle_restore_s)",
					current.idleRestoreS, current.ledCeiling, light ? "" : " (not confirmed)",
					contrast ? "" : " (not confirmed)");
			}

			if (current.mode == Mode::Compensate)
			{
				if (contrastDriven)
				{
					// Leaving script mode for compensate: CNTR back to 50, which
					// compensate assumes. (log_only and disable restore on exit.)
					const bool ok = driver->SendScript("CNTR", 50);
					DebugLog::Log("LED drive: left script mode; contrast set to 50%s", ok ? "" : " (not confirmed)");
					controller.Reset(controller.Light(), 50);
					contrastDriven = false;
				}
				// Only now may compensate start (Should fix 6).
				std::lock_guard<std::mutex> guard(mutex);
				awaitingContrastReset = false;
			}
			setSmoothing(smoothingWanted(), controller.Contrast());
			if (compensating)
			{
				// mode: compensate steps per frame in BeginFrame(); this thread
				// only logs, so the render thread never writes the log.
				std::string log, warning;
				bool sent = false;
				{
					std::lock_guard<std::mutex> guard(mutex);
					warning.swap(pendingWarning);   // review 2026-10-17
					log.swap(pendingLog);
					sent = pendingLogSent;
					pendingLogSent = false;
				}
				if (!warning.empty()) DebugLog::Log("%s", warning.c_str());
				const auto now = std::chrono::steady_clock::now();
				if (!log.empty() && (sent || now - lastLogTime >= std::chrono::seconds(10)))
				{
					DebugLog::Log("%s%s", log.c_str(), applyGain ? "" : " (dry run)");
					lastLogTime = now;
				}
			}
			else if (current.mode == Mode::PictureOnly)
			{
				// Delta 12: no port, no controller; the statistics feed the
				// picture terms in BeginFrame(). A heartbeat in the log only.
				const auto now = std::chrono::steady_clock::now();
				if (now - lastLogTime >= std::chrono::seconds(10))
				{
					double veil = 0.0;
					{
						std::lock_guard<std::mutex> guard(mutex);
						veil = gain.veil;
					}
					DebugLog::Log("LED drive: picture only (RS-232 port not opened; veil %.2f)", veil);
					lastLogTime = now;
				}
			}
			else if (have)
			{
				const ControllerOutput out = controller.Tick(stats);
				// Bug hunt 2026-10-17: repeat a failed write with what the script
				// now believes, at the next tick's start. Review 2026-10-17: after
				// its decision (which never depends on the port) and only for a
				// command this tick does not send itself, so the script's own
				// command stream is unchanged.
				// script_resend_failed_writes off (restore switch): the pure script stream.
				if (!current.scriptResendFailedWrites)
					resendLight = resendContrast = -1;
				if (resendLight >= 0 && (out.lightCommand >= 0 || CustomDimEngaged()))
					resendLight = -1;
				if (resendContrast >= 0 && !out.contrastSchedule.empty())
					resendContrast = -1;
				if (resendLight >= 0)
				{
					logResend("LPW1", controller.Light());
					resendLight = scriptSend("LPW1", controller.Light());
					sendsDone = std::chrono::steady_clock::now();
				}
				if (resendContrast >= 0)
				{
					logResend("CNTR", controller.Contrast());
					resendContrast = scriptSend("CNTR", controller.Contrast());
					sendsDone = std::chrono::steady_clock::now();
				}
				if (current.scriptPacing)
				{
					// script_pacing: script (fix under test, owner 2026-10-15): the
					// script's own rhythm - its screenshot time before acting,
					// each echo's time, its sleeps, and the next loop at once.
					std::vector<ScriptSend> sends;
					const int lengthMs = PaceScriptTick(out, current.scriptMeasureMs, current.scriptEchoMs, sends);
					const double tickStartMs = NowMs() -
						std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tickStart).count();
					bool smoothNow = false;
					{
						std::lock_guard<std::mutex> guard(mutex);
						smoothNow = smoothing;
						if (smoothNow && !out.contrastSchedule.empty())
						{
							std::vector<std::pair<double, int>> landings;
							for (const ScriptSend& send : sends)
								if (!send.light)
									landings.push_back({ tickStartMs + send.offsetMs + projector.cntrLatencyMs, send.value });
							smoother.Plan(landings);
						}
					}
					for (const ScriptSend& send : sends)
					{
						if (!WaitUntil(std::max(sendsDone, tickStart + std::chrono::milliseconds(send.offsetMs)), generation))
							break;
						if (send.light)
						{
							if (CustomDimEngaged()) continue;
							resendLight = scriptSend("LPW1", send.value);
						}
						else
						{
							resendContrast = scriptSend("CNTR", send.value);
							if (send.value != 50) contrastDriven = true;
							if (smoothNow)
							{
								std::lock_guard<std::mutex> guard(mutex);
								smoother.Sent(NowMs() + projector.cntrLatencyMs);
							}
						}
						sendsDone = std::chrono::steady_clock::now();
					}
					scriptFloor = tickStart + std::chrono::milliseconds(std::max(lengthMs, 10));   // never a spin
					const auto now = std::chrono::steady_clock::now();
					if (out.lightCommand >= 0 || !out.contrastSchedule.empty() ||
						now - lastLogTime >= std::chrono::seconds(10))
					{
						if (out.log != lastLog || now - lastLogTime >= std::chrono::seconds(10))
							DebugLog::Log("%s (script pacing)%s", out.log.c_str(), applyGain ? "" : " (dry run)");
						lastLog = out.log;
						lastLogTime = now;
					}
					{
						std::lock_guard<std::mutex> guard(mutex);
						++ticks;
					}
					// No tick_ms floor: the script starts its next loop at once.
					WaitUntil(std::max(sendsDone, scriptFloor), generation);
					continue;
				}
				// Step 7b: the script's own commands, sent synchronously in its
				// own timing - LPW1 at the tick start, then each CNTR after the
				// script's sleep, counted from the end of the previous send
				// (the planner's step 7 answer 3). Every scheduled CNTR is sent,
				// repeats included, as the script did.
				bool smoothNow = false;
				{
					std::lock_guard<std::mutex> guard(mutex);
					smoothNow = smoothing;
					if (smoothNow && !out.contrastSchedule.empty())
					{
						// Planned landings, moved to the real times as each goes out.
						std::vector<std::pair<double, int>> landings;
						const double base = NowMs() + projector.cntrLatencyMs;
						for (const ContrastStep& step : out.contrastSchedule)
							landings.push_back({ base + step.offsetMs, step.contrast });
						smoother.Plan(landings);
					}
				}
				// Never LPW1 while the custom dim is on (Reconfigure leaves it
				// before script mode runs; this only guards the order).
				if (out.lightCommand >= 0 && !CustomDimEngaged())
				{
					resendLight = scriptSend("LPW1", out.lightCommand);
					sendsDone = std::chrono::steady_clock::now();
					// The script slept 200 ms after LPW1 even with no CNTR to
					// follow; keep that with a tick_ms below it (audit note 4).
					if (out.contrastSchedule.empty())
						scriptFloor = sendsDone + std::chrono::milliseconds(out.durationMs);
				}
				int previousOffset = 0;
				for (const ContrastStep& step : out.contrastSchedule)
				{
					// New settings end the tick at once (Should fix 6): the
					// rest of a schedule is not sent under the new mode.
					if (!WaitUntil(sendsDone + std::chrono::milliseconds(step.offsetMs - previousOffset), generation))
						break;
					previousOffset = step.offsetMs;
					resendContrast = scriptSend("CNTR", step.contrast);
					sendsDone = std::chrono::steady_clock::now();
					if (step.contrast != 50) contrastDriven = true;
					if (smoothNow)
					{
						std::lock_guard<std::mutex> guard(mutex);
						smoother.Sent(NowMs() + projector.cntrLatencyMs);
					}
				}
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
			// The next tick: tick_ms after this one started, and never before
			// this tick's last command went out (or the script's sleep after
			// an LPW1). New settings end the wait; a stop ends the loop at its
			// top.
			WaitUntil(std::max({ tickStart + std::chrono::milliseconds(current.tickMs), sendsDone, scriptFloor }),
				generation);
		}

		{
			std::lock_guard<std::mutex> guard(mutex);
			gain.active = false;
			gain.gain = 1.0;
			smoothing = false;
			compensating = false;
			contrastDriven = contrastDriven || assistContrastDriven;
		}
		// Bug hunt 2026-10-17: also restore CNTR when the port last saw
		// anything but 50 (a failed or stray write), whatever the flags say.
		{
			const DriverStatus status = driver->Status();
			if (status.contrastBusy || (status.lastSentContrast >= 0 && status.lastSentContrast != 50))
				contrastDriven = true;
		}
		if (driving)
			RestoreFullLight(*driver, current.ledCeiling,
				current.ftbCustomDim != FtbCustomDim::Off || fadeToBlack != FadeToBlack::Normal, contrastDriven);
		driver->Close();
	}
}
