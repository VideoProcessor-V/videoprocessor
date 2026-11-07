#include "pch.h"
#include <VP-V/modules/input/signal/HdrInference.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
#include <VP-V/modules/input/source_analysis/lane/shared/SourceStats.h>

#include <algorithm>
#include <cmath>

const char* ToString(HdrInferenceState state)
{
	switch (state)
	{
	case HdrInferenceState::Off: return "off";
	case HdrInferenceState::NotEligible: return "not-eligible";
	case HdrInferenceState::Waiting: return "waiting";
	case HdrInferenceState::InferredPq: return "inferred-pq";
	case HdrInferenceState::VetoedSdr: return "vetoed-sdr";
	case HdrInferenceState::Suspended: return "suspended";
	default: return "unknown";
	}
}

const char* ToString(HdrInferenceEvent event)
{
	switch (event)
	{
	case HdrInferenceEvent::None: return "none";
	case HdrInferenceEvent::CandidateSeen: return "candidate-seen";
	case HdrInferenceEvent::Inferred: return "inferred";
	case HdrInferenceEvent::CandidateCleared: return "candidate-cleared";
	case HdrInferenceEvent::Ended: return "ended";
	case HdrInferenceEvent::NewEpoch: return "new-epoch";
	case HdrInferenceEvent::ShotEvidence: return "shot-evidence";
	case HdrInferenceEvent::VetoArmed: return "veto-armed";
	case HdrInferenceEvent::VetoAtCut: return "veto-at-cut";
	case HdrInferenceEvent::VetoWithoutCut: return "veto-without-cut";
	case HdrInferenceEvent::Suspended: return "suspended";
	default: return "unknown";
	}
}

HdrInferenceEligibility EvaluateHdrInferenceEligibility(
	const HdrInferenceSignal& signal)
{
	HdrInferenceEligibility result;
	if (!signal.enabled)
		result.reason = "newlldv off";
	else if (!signal.valid)
		result.reason = "capture state invalid";
	else if (signal.dolbyVisionTunnel)
		result.reason = "Dolby Vision tunnel owns the signal";
	else if (signal.hdrInfoFrame)
		result.reason = "source sends an HDR InfoFrame";
	else if (signal.eotf != EOTF::SDR)
		result.reason = "source EOTF is not SDR";
	else if (signal.colorspace != ColorSpace::BT_2020)
		result.reason = "colorimetry is not BT.2020";
	else if (signal.bitDepth <= 0)
		result.reason = "source bit depth unknown";
	else if (signal.bitDepth < 10 && !signal.lldvIndicated && !signal.dvTunnelHandedOff)
		result.reason = "source is below 10-bit";
	else if (signal.explicitSettingsForbid)
		result.reason = "explicit hdr_colorspace REC709 forbids HDR10 inference";
	else
	{
		result.eligible = true;
		// DV input fixes 2026-10-18: 8-bit qualifies only with an LLDV
		// indication (the Dolby VSIF, or the DV tunnel hand-off on a DV
		// input); see HdrInferenceSignal.
		result.reason = signal.bitDepth >= 10 ? "BT.2020, 10/12-bit, no HDR InfoFrame" :
			signal.lldvIndicated ? "BT.2020, 8-bit, no HDR InfoFrame, Dolby VSIF says LLDV" :
			"BT.2020, 8-bit, no HDR InfoFrame, not a DV tunnel on a DV input (handed off)";
	}
	return result;
}

HdrInferenceEligibility EvaluateOriginalNewLldvEligibility(
	const HdrInferenceSignal& signal)
{
	HdrInferenceEligibility result;
	if (!signal.enabled)
		result.reason = "newlldv off";
	else if (!signal.valid)
		result.reason = "capture state invalid";
	else if (signal.colorspace != ColorSpace::BT_2020)
		result.reason = "colorimetry is not BT.2020 (original heuristic)";
	else if (signal.eotf != EOTF::SDR)
		result.reason = "source EOTF is not SDR (original heuristic)";
	else if (signal.hdrInfoFrame)
		result.reason = "source sends an HDR InfoFrame (original heuristic)";
	else
	{
		result.eligible = true;
		result.reason = "BT.2020 / SDR, no HDR InfoFrame (original heuristic)";
	}
	return result;
}

void HdrInferenceTracker::Reset()
{
	m_state = HdrInferenceState::Off;
	m_reason = "newlldv off";
	m_signalKey = 0;
	m_candidateSinceMs = 0;
	m_epochFlips = 0;
	m_suspendedSinceMs = 0;
	m_resumedAfterGap = false;
	ResetShots();
}

void HdrInferenceTracker::ResetShots()
{
	m_havePicture = false;
	m_lastPictureMs = 0;
	m_shotEvidenceMs = 0;
	m_shotCounted = false;
	m_shotsWithEvidence = 0;
	m_vetoArmed = false;
	m_vetoArmedMs = 0;
}

void HdrInferenceTracker::StartEpoch(uint64_t nowMs, uint64_t signalKey)
{
	++m_epoch;
	m_signalKey = signalKey;
	m_candidateSinceMs = nowMs;
	m_epochFlips = 0;
	m_resumedAfterGap = false;
	ResetShots();
}

HdrInferenceUpdate HdrInferenceTracker::Flip(HdrInferenceEvent event)
{
	HdrInferenceUpdate update;
	update.event = event;
	update.effectiveChanged = m_state == HdrInferenceState::InferredPq;
	m_state = HdrInferenceState::VetoedSdr;
	m_reason = event == HdrInferenceEvent::VetoAtCut ?
		"SDR veto at a cut" : "SDR veto without a cut";
	++m_epochFlips;
	m_vetoArmed = false;
	return update;
}

HdrInferenceUpdate HdrInferenceTracker::OnSignal(
	const HdrInferenceSignal& signal, uint64_t nowMs)
{
	HdrInferenceUpdate update;
	const HdrInferenceEligibility eligibility = m_options.originalHeuristic ?
		EvaluateOriginalNewLldvEligibility(signal) : EvaluateHdrInferenceEligibility(signal);
	if (!eligibility.eligible)
	{
		// Bug hunt 37 S1: a PQ epoch whose capture signal merely went invalid (an HDMI re-sync)
		// is suspended, not ended, so the same source coming back resumes PQ at once. VP's
		// inference only; any other reason (or the window running out) ends it as before.
		if (!m_options.originalHeuristic && signal.enabled && !signal.valid &&
			m_options.resumeWindowMs > 0)
		{
			if (m_state == HdrInferenceState::InferredPq)
			{
				m_state = HdrInferenceState::Suspended;
				m_suspendedSinceMs = nowMs;
				m_reason = "suspended: capture state invalid; PQ resumes if the signal returns eligible";
				ResetShots();
				update.event = HdrInferenceEvent::Suspended;
				update.effectiveChanged = true;
				return update;
			}
			if (m_state == HdrInferenceState::Suspended)
			{
				if (nowMs - m_suspendedSinceMs < m_options.resumeWindowMs)
					return update;
				m_state = HdrInferenceState::NotEligible;
				m_reason = "capture state invalid (signal gap longer than the resume window)";
				update.event = HdrInferenceEvent::Ended;
				return update;
			}
		}
		if (m_state == HdrInferenceState::Suspended)
		{
			// Back, but not eligible (or the suspension no longer applies): the epoch ends.
			m_state = signal.enabled ? HdrInferenceState::NotEligible : HdrInferenceState::Off;
			m_reason = eligibility.reason;
			ResetShots();
			update.event = HdrInferenceEvent::Ended;
			return update;
		}
		const bool wasInferred = IsInferredPq();
		if (IsCandidate())
			update.event = m_state == HdrInferenceState::Waiting ?
				HdrInferenceEvent::CandidateCleared : HdrInferenceEvent::Ended;
		update.effectiveChanged = wasInferred;
		m_state = signal.enabled ?
			HdrInferenceState::NotEligible : HdrInferenceState::Off;
		m_reason = eligibility.reason;
		ResetShots();
		return update;
	}

	if (m_state == HdrInferenceState::Suspended)
	{
		if (nowMs - m_suspendedSinceMs < m_options.resumeWindowMs)
		{
			// Bug hunt 37 S1: the signal is back and eligible within the window. A new epoch
			// (as a valid-to-valid mode change) that stays PQ, with fresh veto evidence.
			StartEpoch(nowMs, signal.signalKey);
			m_state = HdrInferenceState::InferredPq;
			m_reason = "inferred (resumed after a signal gap)";
			m_resumedAfterGap = true;
			update.event = HdrInferenceEvent::NewEpoch;
			update.effectiveChanged = true;
			return update;
		}
		m_state = HdrInferenceState::NotEligible;   // too late: stabilise from scratch below
	}

	if (!IsCandidate())
	{
		StartEpoch(nowMs, signal.signalKey);
		m_state = HdrInferenceState::Waiting;
		m_reason = "stabilising";
		update.event = HdrInferenceEvent::CandidateSeen;
		return update;
	}

	// Bill's original heuristic has no signal epochs: a mode change while still a candidate keeps it.
	if (!m_options.originalHeuristic && signal.signalKey != m_signalKey)
	{
		// Mode/rate/format change while still eligible: a new epoch. A PQ
		// epoch stays PQ (no flicker through SDR); a vetoed or still waiting
		// epoch stabilises again before PQ is inferred.
		const HdrInferenceState previous = m_state;
		StartEpoch(nowMs, signal.signalKey);
		update.event = HdrInferenceEvent::NewEpoch;
		if (previous == HdrInferenceState::InferredPq)
		{
			m_state = HdrInferenceState::InferredPq;
			m_reason = "inferred (new epoch)";
		}
		else
		{
			m_state = HdrInferenceState::Waiting;
			m_reason = "stabilising";
		}
		return update;
	}

	return OnTick(nowMs);
}

HdrInferenceUpdate HdrInferenceTracker::OnTick(uint64_t nowMs)
{
	HdrInferenceUpdate update;
	if (m_state == HdrInferenceState::Suspended &&
		nowMs - m_suspendedSinceMs >= m_options.resumeWindowMs)
	{
		m_state = HdrInferenceState::NotEligible;
		m_reason = "capture state invalid (signal gap longer than the resume window)";
		update.event = HdrInferenceEvent::Ended;
		return update;
	}
	if (m_state == HdrInferenceState::Waiting &&
		nowMs - m_candidateSinceMs >= m_options.confirmMs)
	{
		m_state = HdrInferenceState::InferredPq;
		m_reason = "inferred";
		update.event = HdrInferenceEvent::Inferred;
		update.effectiveChanged = true;
		return update;
	}
	if (m_state == HdrInferenceState::InferredPq && VetoOn() &&
		m_vetoArmed && nowMs - m_vetoArmedMs >= m_options.noCutFallbackMs)
		return Flip(HdrInferenceEvent::VetoWithoutCut);
	return update;
}

HdrInferenceUpdate HdrInferenceTracker::OnPicture(
	const HdrPictureObservation& picture, uint64_t nowMs)
{
	HdrInferenceUpdate update;
	if (m_state != HdrInferenceState::InferredPq || !VetoOn())
	{
		m_havePicture = false;
		return update;
	}
	if (!picture.measured)
		return update;

	const uint64_t dt = m_havePicture ?
		std::min<uint64_t>(nowMs - m_lastPictureMs, m_options.maxSampleGapMs) : 0;
	m_lastPictureMs = nowMs;
	m_havePicture = true;

	if (picture.cut)
	{
		if (m_vetoArmed)
			return Flip(HdrInferenceEvent::VetoAtCut);
		m_shotEvidenceMs = 0;
		m_shotCounted = false;
	}

	if (picture.activePicture && picture.sdrEvidence)
		m_shotEvidenceMs += dt;

	if (!m_shotCounted && m_shotEvidenceMs >= m_options.shotEvidenceMs)
	{
		m_shotCounted = true;
		++m_shotsWithEvidence;
		update.event = HdrInferenceEvent::ShotEvidence;
	}
	// One uncut shot that keeps showing SDR evidence (a static SDR menu)
	// counts again once it has done so for the no-cut interval.
	else if (m_shotCounted && !m_vetoArmed &&
		m_shotEvidenceMs >= m_options.shotEvidenceMs + m_options.noCutFallbackMs)
	{
		++m_shotsWithEvidence;
		m_shotEvidenceMs = m_options.shotEvidenceMs;
		update.event = HdrInferenceEvent::ShotEvidence;
	}

	if (!m_vetoArmed && m_shotsWithEvidence >= m_options.shotsRequired)
	{
		m_vetoArmed = true;
		m_vetoArmedMs = nowMs;
		update.event = HdrInferenceEvent::VetoArmed;
		m_reason = "SDR veto armed; waiting for a cut";
	}
	else if (m_vetoArmed && nowMs - m_vetoArmedMs >= m_options.noCutFallbackMs)
		return Flip(HdrInferenceEvent::VetoWithoutCut);
	return update;
}

HdrVetoFrameStats MeasureHdrVetoGrid(const uint16_t* grid, int columns, int rows,
	const HdrVetoThresholds& thresholds)
{
	HdrVetoFrameStats stats;
	if (!grid || columns <= 0 || rows <= 0)
		return stats;
	stats.measured = true;
	stats.samples = static_cast<uint32_t>(columns) * static_cast<uint32_t>(rows);

	std::array<double, HdrVetoFrameStats::GRID_COLUMNS * HdrVetoFrameStats::GRID_ROWS> sums{};
	std::array<uint32_t, HdrVetoFrameStats::GRID_COLUMNS * HdrVetoFrameStats::GRID_ROWS> counts{};
	std::array<uint32_t, HdrVetoFrameStats::HISTOGRAM_BINS> bins{};
	const uint32_t activeThreshold = std::max<uint32_t>(1, static_cast<uint32_t>(
		std::ceil(thresholds.activeRowFraction * columns)));
	int firstActive = -1;
	int lastActive = -1;
	for (int y = 0; y < rows; ++y)
	{
		const uint16_t* row = grid + static_cast<size_t>(y) * columns;
		const int cy = y * HdrVetoFrameStats::GRID_ROWS / rows;
		uint32_t aboveBlack = 0;
		for (int x = 0; x < columns; ++x)
		{
			const uint16_t code = std::min<uint16_t>(row[x], 1023);
			const int cx = x * HdrVetoFrameStats::GRID_COLUMNS / columns;
			const size_t cell = static_cast<size_t>(cy) * HdrVetoFrameStats::GRID_COLUMNS + cx;
			sums[cell] += code;
			++counts[cell];
			++bins[VpUnit::SourceStats::HistBin10(code, static_cast<uint32_t>(bins.size()))];   // U6 bin law
			if (code > thresholds.blackCode)
				++aboveBlack;
		}
		if (aboveBlack >= activeThreshold)
		{
			if (firstActive < 0)
				firstActive = y;
			lastActive = y;
		}
	}
	for (size_t i = 0; i < sums.size(); ++i)
		stats.grid[i] = counts[i] ? static_cast<float>(sums[i] / counts[i]) : 0.0f;
	VpUnit::SourceStats::Fractions<float>(bins.data(), bins.size(), stats.samples, stats.histogram.data());

	if (firstActive < 0 ||
		lastActive - firstActive + 1 < thresholds.minimumActiveRows * rows)
		return stats;

	VpUnit::SourceStats::Histogram10 active;   // U6: the active rows' 10-bit histogram
	for (int y = firstActive; y <= lastActive; ++y)
	{
		const uint16_t* row = grid + static_cast<size_t>(y) * columns;
		for (int x = 0; x < columns; ++x)
			active.Add(row[x]);
	}
	stats.activePicture = true;
	stats.activeSamples = static_cast<uint32_t>(lastActive - firstActive + 1) *
		static_cast<uint32_t>(columns);
	stats.atOrAboveHigh = static_cast<uint32_t>(active.CountAtOrAbove(thresholds.highCode));
	stats.fractionAtHigh = static_cast<double>(stats.atOrAboveHigh) / stats.activeSamples;
	const double target = 0.999 * stats.activeSamples;
	const int p999 = active.CodeReaching(target);
	if (p999 >= 0)
		stats.p999 = static_cast<uint16_t>(p999);
	stats.sdrEvidence = stats.fractionAtHigh >= thresholds.highFraction ||
		stats.p999 >= thresholds.p999Code;
	return stats;
}

bool SampleHdrVetoGrid(const AnalysisLumaSource& source, int columns, int rows,
	std::vector<uint16_t>& grid)
{
	grid.clear();
	if (!source.IsValid() || columns <= 0 || rows <= 0 ||
		source.width <= 0 || source.height <= 0)
		return false;
	grid.reserve(static_cast<size_t>(columns) * rows);
	for (int y = 0; y < rows; ++y)
	{
		const int sy = static_cast<int>((2LL * y + 1) * source.height / (2LL * rows));
		for (int x = 0; x < columns; ++x)
		{
			const int sx = static_cast<int>((2LL * x + 1) * source.width / (2LL * columns));
			uint16_t luma = 0;
			if (!source.SampleLuma(sx, sy, luma))   // WP-23: the shared decode when ready
			{
				grid.clear();
				return false;
			}
			grid.push_back(luma);
		}
	}
	return true;
}

bool HdrVetoCutDetector::Observe(const HdrVetoFrameStats& stats)
{
	if (!stats.measured)
		return false;
	bool cut = false;
	m_lastGrid = 0.0f;
	m_lastHistogram = 0.0f;
	if (m_havePrevious)
	{
		if (m_previousActive != stats.activePicture)
			cut = true;   // entering or leaving black
		else if (stats.activePicture)
		{
			// U7 / U6: the mean cell difference and the half-L1 histogram distance (the signature histograms are
			// already fractions: total 1), judged by the unit's grid + histogram rule with this detector's thresholds.
			const float grid = VpUnit::CutFadeJudge::MeanAbsDifference(stats.grid.data(), m_previous.grid.data(), stats.grid.size());
			const float distance = VpUnit::SourceStats::HalfL1Distance<float>(stats.histogram.data(), m_previous.histogram.data(),
				stats.histogram.size(), 1.0f, 1.0f);
			m_lastGrid = grid;
			m_lastHistogram = distance;
			VpUnit::CutFadeJudge::GridHistThresholds limits;
			limits.gridDifference = GRID_DIFFERENCE;
			limits.histogramDistance = HISTOGRAM_DISTANCE;
			limits.histogramDistanceAlone = HISTOGRAM_DISTANCE_ALONE;
			cut = VpUnit::CutFadeJudge::GridHistCut(grid, distance, limits);
		}
	}
	m_previous = stats;
	m_previousActive = stats.activePicture;
	m_havePrevious = true;
	return cut;
}
