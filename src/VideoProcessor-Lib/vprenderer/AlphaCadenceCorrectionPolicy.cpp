#include <pch.h>
#include <DisplayRefreshRatePolicy.h>

#include "AlphaCadenceCorrectionPolicy.h"
#include <GeneralRateLog.h>

#include <algorithm>
#include <cmath>

void AlphaArrivalRateEstimator::Reset()
{
	*this = AlphaArrivalRateEstimator();
}

void AlphaArrivalRateEstimator::Add(double arrivalSeconds, double rateHintHz)
{
	if (!(arrivalSeconds > 0.0) || !std::isfinite(arrivalSeconds))
		return;
	static GeneralRate::FallbackNotice fallback;   // fixed-constant batch B7 2026-10-18: logged
	const bool hinted = GeneralRate::NoteFallback(fallback, CaptureRateBounds::Accepts(rateHintHz),
		"arrival-rate estimator (no capture rate hint)", 60.0);
	const double rate = hinted ? rateHintHz : 60.0;
	if (m_started && arrivalSeconds == m_last)
		return;    // the same frame again (a film frame's update): no new period
	if (m_started && arrivalSeconds < m_last)
		Reset();   // a clock step back: start over
	if (m_started && hinted && m_hintHz > 0.0 &&
		std::abs(rateHintHz - m_hintHz) > kHintChangeShare * m_hintHz)
		Reset();   // Bug hunt 3 F3: another rate (a pulldown lock change): start over
	if (hinted)
		m_hintHz = rateHintHz;
	double periods = 1.0;
	if (m_started && hinted)
	{
		// The periods since the last frame, counted with the hint (the card's rate is
		// within ppm of the truth, so rounding finds them): a missed or dropped frame
		// advances the index by its periods instead of bending the line. The slope
		// itself stays measured on QPC.
		const double elapsed = (arrivalSeconds - m_last) * rateHintHz;
		if (elapsed > kMaximumGapSeconds * rateHintHz)
			Reset();   // a break, not missed frames
		else
			periods = (std::max)(1.0, std::floor(elapsed + 0.5));
	}
	if (!m_started)
	{
		m_started = true;
		m_t0 = arrivalSeconds;
		m_index = 0.0;
	}
	else
	{
		m_index += periods;
	}
	m_last = arrivalSeconds;
	const double keep = std::exp(-1.0 / (rate * kWindowSeconds));
	const double x = m_index;
	const double y = arrivalSeconds - m_t0;
	m_w = m_w * keep + 1.0;
	m_x = m_x * keep + x;
	m_y = m_y * keep + y;
	m_xx = m_xx * keep + x * x;
	m_xy = m_xy * keep + x * y;
}

double AlphaArrivalRateEstimator::RateHz() const
{
	if (!m_started || m_last - m_t0 < kMinimumSpanSeconds || m_w < 2.0)
		return 0.0;
	const double meanX = m_x / m_w;
	const double meanY = m_y / m_w;
	const double sxx = m_xx / m_w - meanX * meanX;
	const double sxy = m_xy / m_w - meanX * meanY;
	if (!(sxx > 0.0) || !(sxy > 0.0))
		return 0.0;
	const double hz = sxx / sxy;   // 1 / (seconds per frame)
	return CaptureRateBounds::Accepts(hz) ? hz : 0.0;
}

uint32_t AlphaCadenceCorrectionPolicy::FramesFor(double seconds, double rateHz,
	uint32_t legacy, bool timingFixes)
{
	if (!timingFixes || !CaptureRateBounds::Accepts(rateHz))
		return legacy;
	return static_cast<uint32_t>((std::max)(1.0, std::ceil(seconds * rateHz)));
}

double AlphaCadenceCorrectionPolicy::FilterAlpha(double rateHz, bool timingFixes)
{
	if (!timingFixes || !CaptureRateBounds::Accepts(rateHz))
		return RATE_FILTER_ALPHA;
	return 1.0 - std::exp(-1.0 / (rateHz * RATE_FILTER_SECONDS));
}

void AlphaCadenceCorrectionPolicy::Reset(uint64_t generation)
{
	m_arrivalRate.Reset();
	m_generation = generation;
	m_phaseFrames = 0.0;
	m_stableSamples = 0;
	m_rateFilterSamples = 0;
	m_plannedFrames = 0;
	m_cooldownFrames = 0;
	m_lastSceneEventId = 0;
	m_verificationPending = false;
	m_verificationAction = AlphaCadenceAction::None;
	m_verificationDebt = 0;
	m_verificationPresentId = 0;
	m_lastVerificationValid = false;
	m_lastVerificationSucceeded = false;
	m_filteredPhasePerFrame = 0.0;
	m_predictionDirection = AlphaCadenceAction::None;
}

void AlphaCadenceCorrectionPolicy::CancelPendingAction()
{
	if (!m_verificationPending)
		return;
	m_phaseFrames +=
		m_verificationAction == AlphaCadenceAction::Drop ? 1.0 : -1.0;
	m_verificationPending = false;
	m_verificationAction = AlphaCadenceAction::None;
	m_verificationDebt = 0;
	m_verificationPresentId = 0;
}

AlphaCadenceCorrectionDecision AlphaCadenceCorrectionPolicy::Evaluate(
	const AlphaCadenceCorrectionInput& input)
{
	if (input.generation != m_generation)
		Reset(input.generation);

	AlphaCadenceCorrectionDecision decision;
	decision.diagnostic.policyGeneration = input.generation;
	decision.diagnostic.detectorGeneration = input.detectorGeneration;
	decision.diagnostic.presentationGeneration =
		input.presentationGeneration;
	decision.diagnostic.sourceSequence = input.sourceSequence;
	decision.diagnostic.presentationEvidence = input.presentationEvidence;
	decision.diagnostic.captureRateHz = input.captureRateHz;
	decision.diagnostic.displayRateHz = input.displayRateHz;
	decision.diagnostic.queueDepth = input.queueDepth;
	decision.diagnostic.desiredQueueDepth =
		std::max<size_t>(1, input.desiredQueueDepth);
	decision.diagnostic.oldestQueuedAgeMs = input.oldestQueuedAgeMs;
	decision.diagnostic.presentationDebt = input.presentationDebt;
	decision.diagnostic.lastPresentId = input.lastPresentId;
	decision.diagnostic.safeSceneBoundary = input.safeSceneBoundary;
	decision.diagnostic.sceneEventId = input.sceneEventId;
	const bool ratesInRange =
		CaptureRateBounds::Accepts(input.captureRateHz) &&
		DisplayRefreshBounds::Accepts(input.displayRateHz);
	if (!input.enabled ||
		input.presentationEvidence != AlphaPresentationEvidence::Stable ||
		!ratesInRange)
	{
		if (input.timingFixes)
		{
			// Hunt 2 13-timing L6: a disabled policy keeps no phase and no pending
			// action (its -1/+1 reservation would be off by one on re-enable).
			m_phaseFrames = 0.0;
			m_cooldownFrames = 0;
			m_verificationAction = AlphaCadenceAction::None;
			m_verificationDebt = 0;
			m_verificationPresentId = 0;
			m_arrivalRate.Reset();
		}
		m_stableSamples = 0;
		m_rateFilterSamples = 0;
		m_filteredPhasePerFrame = 0.0;
		m_predictionDirection = AlphaCadenceAction::None;
		m_plannedFrames = 0;
		m_verificationPending = false;
		m_lastVerificationValid = false;
		decision.phaseFrames = m_phaseFrames;
		decision.timingStatus = !input.enabled
			? AlphaCadenceTimingStatus::Disabled
			: (input.presentationEvidence != AlphaPresentationEvidence::Stable
				? AlphaCadenceTimingStatus::WaitingForDxgi
				: AlphaCadenceTimingStatus::Measuring);
		decision.blockReason = !input.enabled
			? AlphaCadenceBlockReason::Disabled
			: (input.presentationEvidence != AlphaPresentationEvidence::Stable
				? AlphaCadenceBlockReason::PresentationEvidenceUnavailable
				: AlphaCadenceBlockReason::InvalidRates);
		return decision;
	}

	const bool fixes = input.timingFixes;
	const uint32_t predictionSamples = FramesFor(PREDICTION_SECONDS,
		input.captureRateHz, MINIMUM_PREDICTION_SAMPLES, fixes);
	const uint32_t stableSamples = FramesFor(STABLE_SECONDS,
		input.captureRateHz, MINIMUM_STABLE_SAMPLES, fixes);
	const uint32_t cooldownFrames = FramesFor(COOLDOWN_SECONDS,
		input.captureRateHz, COOLDOWN_FRAMES, fixes);
	// Hunt 2 13-timing M1: with arrival times the capture rate is measured on QPC,
	// the display rate's clock; the card's clock (tens of ppm from QPC) would show
	// a mismatch the queue never sees. Without arrival times, a nominal fallback
	// rate (after a timestamp break) is not a measurement and does not feed the
	// filter.
	double captureRateHz = input.captureRateHz;
	bool updateFilter = true;
	if (fixes)
	{
		if (input.arrivalSeconds > 0.0)
		{
			m_arrivalRate.Add(input.arrivalSeconds, input.captureRateHz);
			captureRateHz = m_arrivalRate.RateHz();
			updateFilter = captureRateHz > 0.0;
		}
		else
		{
			updateFilter = input.captureRateMeasured;
		}
		if (!updateFilter && m_rateFilterSamples == 0)
		{
			decision.phaseFrames = m_phaseFrames;
			decision.timingStatus = AlphaCadenceTimingStatus::Measuring;
			decision.blockReason = AlphaCadenceBlockReason::StabilizingRates;
			return decision;
		}
	}
	const double rawPhasePerFrame = updateFilter ?
		captureRateHz / input.displayRateHz - 1.0 : m_filteredPhasePerFrame;
	decision.diagnostic.rawMismatchPpm =
		rawPhasePerFrame * 1000000.0;
	if (!updateFilter)
	{
		// The filter keeps its value until the rate is measured again.
	}
	else if (m_rateFilterSamples < predictionSamples)
	{
		++m_rateFilterSamples;
		m_filteredPhasePerFrame +=
			(rawPhasePerFrame - m_filteredPhasePerFrame) /
			static_cast<double>(m_rateFilterSamples);
	}
	else
	{
		m_filteredPhasePerFrame +=
			(rawPhasePerFrame - m_filteredPhasePerFrame) *
			FilterAlpha(input.captureRateHz, fixes);
	}
	const double filteredMismatchPpm =
		std::abs(m_filteredPhasePerFrame) * 1000000.0;
	decision.rateFilterSamples = m_rateFilterSamples;
	decision.filteredMismatchPpm = filteredMismatchPpm;
	decision.ratesCompatible =
		filteredMismatchPpm <= MAXIMUM_RATE_MISMATCH_PPM;
	if (!decision.ratesCompatible)
	{
		m_stableSamples = 0;
		m_plannedFrames = 0;
		m_verificationPending = false;
		m_lastVerificationValid = false;
		decision.phaseFrames = m_phaseFrames;
		decision.timingStatus = AlphaCadenceTimingStatus::Incompatible;
		decision.blockReason =
			AlphaCadenceBlockReason::RateMismatchTooLarge;
		return decision;
	}

	if (m_verificationPending &&
		input.lastPresentId != 0 &&
		input.lastPresentId != m_verificationPresentId)
	{
		decision.verificationAction = m_verificationAction;
		m_lastVerificationValid = true;
		m_lastVerificationSucceeded =
			m_verificationAction == AlphaCadenceAction::Drop
				? (m_verificationDebt > 0 &&
					input.presentationDebt + 1 == m_verificationDebt)
				: input.presentationDebt == m_verificationDebt + 1;
		m_verificationPending = false;
		m_cooldownFrames = cooldownFrames;
		decision.verificationCompleted = true;
	}
	decision.lastVerificationValid = m_lastVerificationValid;
	decision.lastVerificationSucceeded = m_lastVerificationSucceeded;
	decision.verificationPending = m_verificationPending;

	if (m_stableSamples < stableSamples)
	{
		++m_stableSamples;
		decision.phaseFrames = m_phaseFrames;
		decision.timingStatus = AlphaCadenceTimingStatus::Measuring;
		decision.blockReason =
			AlphaCadenceBlockReason::StabilizingRates;
		return decision;
	}

	m_phaseFrames += m_filteredPhasePerFrame;
	if (fixes)
	{
		// Hunt 2 13-timing M1: bounded, so an action held back by the queue gates
		// cannot let the phase run on for the whole generation.
		m_phaseFrames = (std::max)(-MAXIMUM_PHASE_FRAMES,
			(std::min)(MAXIMUM_PHASE_FRAMES, m_phaseFrames));
	}
	decision.diagnostic.phaseFrames = m_phaseFrames;
	const double signedFilteredPpm = m_filteredPhasePerFrame * 1000000.0;
	decision.diagnostic.filteredMismatchPpm =
		signedFilteredPpm;
	if (m_predictionDirection == AlphaCadenceAction::None)
	{
		if (signedFilteredPpm >= MINIMUM_PREDICTION_PPM)
			m_predictionDirection = AlphaCadenceAction::Drop;
		else if (signedFilteredPpm <= -MINIMUM_PREDICTION_PPM)
			m_predictionDirection = AlphaCadenceAction::Repeat;
	}
	else if (m_predictionDirection == AlphaCadenceAction::Drop &&
		signedFilteredPpm <= -DIRECTION_REVERSAL_PPM)
	{
		m_predictionDirection = AlphaCadenceAction::Repeat;
	}
	else if (m_predictionDirection == AlphaCadenceAction::Repeat &&
		signedFilteredPpm >= DIRECTION_REVERSAL_PPM)
	{
		m_predictionDirection = AlphaCadenceAction::Drop;
	}

	const bool predictionDirectionAgrees =
		(m_predictionDirection == AlphaCadenceAction::Drop &&
			signedFilteredPpm >= MINIMUM_PREDICTION_PPM) ||
		(m_predictionDirection == AlphaCadenceAction::Repeat &&
			signedFilteredPpm <= -MINIMUM_PREDICTION_PPM);
	if (m_rateFilterSamples >= predictionSamples &&
		predictionDirectionAgrees && !m_verificationPending)
	{
		decision.predictedAction = m_predictionDirection;
		const double phasePerFrame = m_filteredPhasePerFrame;
		const double correctionPhase =
			m_predictionDirection == AlphaCadenceAction::Drop
				? ACTION_PHASE_FRAMES : -ACTION_PHASE_FRAMES;
		const double correctionFrames =
			(correctionPhase - m_phaseFrames) / phasePerFrame;
		decision.secondsUntilCorrection =
			correctionFrames / input.captureRateHz;
		const double planPhase =
			m_predictionDirection == AlphaCadenceAction::Drop
				? PLAN_PHASE_FRAMES : -PLAN_PHASE_FRAMES;
		const double planFrames =
			(planPhase - m_phaseFrames) / phasePerFrame;
		decision.secondsUntilPlan = planFrames / input.captureRateHz;
		decision.predictionValid =
			std::isfinite(decision.secondsUntilCorrection) &&
			std::isfinite(decision.secondsUntilPlan);
	}
	decision.timingStatus = m_verificationPending
		? AlphaCadenceTimingStatus::Verifying
		: (m_rateFilterSamples < predictionSamples
			? AlphaCadenceTimingStatus::Measuring
			: (decision.predictionValid
				? AlphaCadenceTimingStatus::Forecasting
				: AlphaCadenceTimingStatus::Matched));

	const AlphaCadenceAction requested =
		m_phaseFrames >= ACTION_PHASE_FRAMES
			? AlphaCadenceAction::Drop
			: (m_phaseFrames <= -ACTION_PHASE_FRAMES
				? AlphaCadenceAction::Repeat
				: AlphaCadenceAction::None);
	decision.due = requested != AlphaCadenceAction::None;
	decision.planned = std::abs(m_phaseFrames) >= PLAN_PHASE_FRAMES;
	decision.diagnostic.cooldownFrames = m_cooldownFrames;

	if (m_cooldownFrames > 0)
	{
		--m_cooldownFrames;
		decision.phaseFrames = m_phaseFrames;
		decision.blockReason = AlphaCadenceBlockReason::Cooldown;
		return decision;
	}
	if (m_verificationPending)
	{
		decision.phaseFrames = m_phaseFrames;
		decision.blockReason =
			AlphaCadenceBlockReason::VerificationPending;
		return decision;
	}

	if (decision.planned)
		++m_plannedFrames;
	else
		m_plannedFrames = 0;

	if (requested == AlphaCadenceAction::None)
	{
		decision.phaseFrames = m_phaseFrames;
		decision.blockReason = decision.predictionValid
			? AlphaCadenceBlockReason::BeforeDeadline
			: AlphaCadenceBlockReason::NoActionableMismatch;
		return decision;
	}

	const size_t desired = std::max<size_t>(1, input.desiredQueueDepth);
	const uint32_t fallbackFrames = static_cast<uint32_t>(std::max(
		1.0, std::ceil(MAXIMUM_QUEUE_AGE_MS * input.displayRateHz / 1000.0)));
	decision.diagnostic.fallbackFrames = fallbackFrames;
	decision.diagnostic.plannedFrames = m_plannedFrames;
	decision.diagnostic.sceneEventFresh =
		input.sceneEventId != 0 && input.sceneEventId != m_lastSceneEventId;
	const bool sceneAuthorized =
		input.safeSceneBoundary && input.sceneEventId != 0 &&
		input.sceneEventId != m_lastSceneEventId;
	decision.diagnostic.sceneAuthorized = sceneAuthorized;
	decision.diagnostic.fallbackMature =
		m_plannedFrames >= fallbackFrames;
	const bool deadlineAuthorized =
		m_plannedFrames >= fallbackFrames &&
		(requested == AlphaCadenceAction::Repeat ||
			input.oldestQueuedAgeMs >= MAXIMUM_QUEUE_AGE_MS);
	decision.diagnostic.fallbackEligible = deadlineAuthorized;

	if (requested == AlphaCadenceAction::Drop &&
		input.queueDepth <= desired)
	{
		decision.phaseFrames = m_phaseFrames;
		decision.blockReason =
			AlphaCadenceBlockReason::DropQueueNotAboveDesired;
		return decision;
	}
	if (requested == AlphaCadenceAction::Repeat &&
		input.queueDepth >= desired)
	{
		decision.phaseFrames = m_phaseFrames;
		decision.blockReason =
			AlphaCadenceBlockReason::RepeatQueueNotBelowDesired;
		return decision;
	}
	if (requested == AlphaCadenceAction::Drop &&
		input.presentationDebt == 0)
	{
		decision.phaseFrames = m_phaseFrames;
		decision.blockReason =
			AlphaCadenceBlockReason::DropPresentationDebtMissing;
		return decision;
	}
	if (requested == AlphaCadenceAction::Repeat &&
		input.presentationDebt != 0)
	{
		decision.phaseFrames = m_phaseFrames;
		decision.blockReason =
			AlphaCadenceBlockReason::RepeatPresentationDebtPresent;
		return decision;
	}

	if (!sceneAuthorized && !deadlineAuthorized)
	{
		decision.phaseFrames = m_phaseFrames;
		if (m_plannedFrames < fallbackFrames)
			decision.blockReason =
				AlphaCadenceBlockReason::FallbackNotMature;
		else if (requested == AlphaCadenceAction::Drop &&
			input.oldestQueuedAgeMs < MAXIMUM_QUEUE_AGE_MS)
			decision.blockReason =
				AlphaCadenceBlockReason::DropFallbackQueueTooYoung;
		else
			decision.blockReason =
				AlphaCadenceBlockReason::WaitingForFreshScene;
		return decision;
	}

	decision.action = requested;
	decision.deadlineFallback = !sceneAuthorized;
	decision.sceneEventId = sceneAuthorized ? input.sceneEventId : 0;
	m_lastSceneEventId = input.sceneEventId;
	m_phaseFrames += requested == AlphaCadenceAction::Drop ? -1.0 : 1.0;
	m_plannedFrames = 0;
	m_verificationPending = true;
	m_verificationAction = requested;
	m_verificationDebt = input.presentationDebt;
	m_verificationPresentId = input.lastPresentId;
	decision.verificationPending = true;
	decision.phaseFrames = m_phaseFrames;
	return decision;
}
