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

	CompensationGain LedDriveModule::BeginFrame(uint64_t sequence)
	{
		return BeginFrameAt(sequence, NowMs());
	}

	CompensationGain LedDriveModule::BeginFrameAt(uint64_t sequence, double timeMs)
	{
		std::lock_guard<std::mutex> guard(mutex);
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
			StepCompensateLocked(sequence, elapsed);
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
		// furthest it has seen); otherwise the latest exact ones.
		const int latency = LatencyFramesLocked();
		FrameStats target = latest;
		target.cut = pendingCut;
		target.fade = pendingFade;
		const auto first = predictions.upper_bound(sequence);
		const auto forCommand = predictions.upper_bound(sequence + static_cast<uint64_t>(latency));
		if (sequence != 0 && first != forCommand)
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
		const ControllerOutput out = compensator.Tick(target, elapsedMs, haveAhead ? &ahead : nullptr);
		// Queued to the driver's thread, which keeps only the newest and
		// sends it when the command gap allows; the model follows what it
		// actually sends (above).
		if (out.lightCommand >= 0 && applyGain)
			driver->SetLight(out.lightCommand);
		policy = out;
		gain.knee = out.knee;
		++ticks;
		if (out.lightCommand >= 0 || pendingLog.empty())
		{
			pendingLog = out.log;
			pendingLogSent = out.lightCommand >= 0;
		}
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
		BringToKnownState(*opened, led.ledCeiling);
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

		ScriptController controller(led);
		controller.Reset(led.ledCeiling, 50);
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
			if (!WaitUntil(tickStart + std::chrono::milliseconds(std::max(led.tickMs, durationMs))))
				break;
		}

		{
			std::lock_guard<std::mutex> guard(mutex);
			gain.active = false;
			gain.gain = 1.0;
			scheduledGains.clear();
			compensating = false;
		}
		if (driving) RestoreFullLight(*driver, led.ledCeiling);
		driver->Close();
	}
}
