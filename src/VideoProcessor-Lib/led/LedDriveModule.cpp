#include <pch.h>
#include "LedDriveModule.h"

#include "../DebugLog.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

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

	int LedDriveModule::CutLevelLocked() const
	{
		return led.cutCntrLevel >= 50 ? led.cutCntrLevel : DmdClipLocked();
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
			}
			compensating = nowCompensating;
			applyGain = live && led.mode != Mode::LogOnly;
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
			if (status.contrastSends != seenContrastSends && status.lastSentContrast >= 0)
				contrastCommands.push_back({ frameIndex, status.lastSentContrast });
			seenContrastSends = status.contrastSends;
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
		while (!commands.empty() && commands.front().frame + latency <= frameIndex)
		{
			modelledLevel = commands.front().level;
			commands.pop_front();
			ledModel.Land(LedFractionLocked(modelledLevel));
		}
		// 7c-1: then the LED's light settles (led_settle_ms; 0 = at once).
		ledModel.Step(elapsed);
		const int contrastLatency = LatencyFramesForLocked(projector.cntrLatencyMs);
		while (!contrastCommands.empty() && contrastCommands.front().frame + contrastLatency <= frameIndex)
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
		// CNTR assist (7c-1): CNTR follows the LED's modelled light for what
		// VP's gain cannot do within the highlight tolerance, as a command
		// only when the paired whole value changes. The light used is the
		// lower of now and cntr_latency_ms ahead (heading for the level just
		// commanded), so CNTR rises no later, and falls no earlier, than the
		// LED needs. Not during a fade's gain hold or the custom dim.
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
			const int level = CutLevelLocked();
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
		if (allowed && contrastCommand < 0)
		{
			// CNTR is paired with the LED's modelled light now: never above the
			// paired value for the modelled LED (policy 3.4; planner's 7c-2
			// answer 4). While the scene has highlights (VP's headroom is
			// limited), CNTR stops at dmd_clip_cntr: above it more CNTR adds no
			// highlight light (the highlight floor keeps the LED where that
			// suffices). Feeding the clip into the headroom instead would make
			// each CNTR step ask for the next.
			const int limit = out.vpHeadroom < 1e8 ? DmdClipLocked() : -1;
			contrastCommand = pairing.Update(1.0 / std::max(1e-6, ledModel.Value()), out.vpHeadroom, limit);
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

	bool LedDriveModule::CustomDimEngaged() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return fadeToBlack != FadeToBlack::Normal;
	}

	CompensationGain LedDriveModule::GainLocked() const
	{
		CompensationGain current = gain;
		if (compensating && applyGain && !awaitingContrastReset)
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
		BringToKnownState(*opened, led.ledCeiling, led.ftbCustomDim != FtbCustomDim::Off,
			led.mode == Mode::Script);
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
		// Set once script mode has sent a CNTR other than 50: the exit, or a
		// switch to compensate, puts it back to 50.
		bool contrastDriven = false;
		bool restoreAssist = false;
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
		setSmoothing(current.mode == Mode::Script && current.scriptSmoothing && applyGain, 50);
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
				have = haveStats;
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
			setSmoothing(current.mode == Mode::Script && current.scriptSmoothing && applyGain,
				controller.Contrast());
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
					driver->SendScript("LPW1", out.lightCommand);
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
					driver->SendScript("CNTR", step.contrast);
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
		if (driving)
			RestoreFullLight(*driver, current.ledCeiling,
				current.ftbCustomDim != FtbCustomDim::Off || fadeToBlack != FadeToBlack::Normal, contrastDriven);
		driver->Close();
	}
}
