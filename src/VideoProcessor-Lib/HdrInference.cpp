#include "pch.h"
#include "HdrInference.h"

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

void HdrInferenceTracker::Reset()
{
	m_state = HdrInferenceState::Off;
	m_reason = "newlldv off";
	m_signalKey = 0;
	m_candidateSinceMs = 0;
	m_epochFlips = 0;
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
	const HdrInferenceEligibility eligibility =
		EvaluateHdrInferenceEligibility(signal);
	if (!eligibility.eligible)
	{
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

	if (!IsCandidate())
	{
		StartEpoch(nowMs, signal.signalKey);
		m_state = HdrInferenceState::Waiting;
		m_reason = "stabilising";
		update.event = HdrInferenceEvent::CandidateSeen;
		return update;
	}

	if (signal.signalKey != m_signalKey)
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
	if (m_state == HdrInferenceState::Waiting &&
		nowMs - m_candidateSinceMs >= m_options.confirmMs)
	{
		m_state = HdrInferenceState::InferredPq;
		m_reason = "inferred";
		update.event = HdrInferenceEvent::Inferred;
		update.effectiveChanged = true;
		return update;
	}
	if (m_state == HdrInferenceState::InferredPq && m_options.sdrVeto &&
		m_vetoArmed && nowMs - m_vetoArmedMs >= m_options.noCutFallbackMs)
		return Flip(HdrInferenceEvent::VetoWithoutCut);
	return update;
}

HdrInferenceUpdate HdrInferenceTracker::OnPicture(
	const HdrPictureObservation& picture, uint64_t nowMs)
{
	HdrInferenceUpdate update;
	if (m_state != HdrInferenceState::InferredPq || !m_options.sdrVeto)
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
			++bins[std::min<int>(code / 64, HdrVetoFrameStats::HISTOGRAM_BINS - 1)];
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
	for (size_t i = 0; i < bins.size(); ++i)
		stats.histogram[i] = static_cast<float>(bins[i]) / stats.samples;

	if (firstActive < 0 ||
		lastActive - firstActive + 1 < thresholds.minimumActiveRows * rows)
		return stats;

	std::array<uint32_t, 1024> histogram{};
	for (int y = firstActive; y <= lastActive; ++y)
	{
		const uint16_t* row = grid + static_cast<size_t>(y) * columns;
		for (int x = 0; x < columns; ++x)
			++histogram[std::min<uint16_t>(row[x], 1023)];
	}
	stats.activePicture = true;
	stats.activeSamples = static_cast<uint32_t>(lastActive - firstActive + 1) *
		static_cast<uint32_t>(columns);
	for (int code = thresholds.highCode; code < 1024; ++code)
		stats.atOrAboveHigh += histogram[code];
	stats.fractionAtHigh = static_cast<double>(stats.atOrAboveHigh) / stats.activeSamples;
	const double target = 0.999 * stats.activeSamples;
	double cumulative = 0.0;
	for (int code = 0; code < 1024; ++code)
	{
		cumulative += histogram[code];
		if (cumulative >= target)
		{
			stats.p999 = static_cast<uint16_t>(code);
			break;
		}
	}
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
			AnalysisLumaSample sample;
			if (!source.Sample(sx, sy, sample))
			{
				grid.clear();
				return false;
			}
			grid.push_back(sample.luma);
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
			float grid = 0.0f;
			for (size_t i = 0; i < stats.grid.size(); ++i)
				grid += std::fabs(stats.grid[i] - m_previous.grid[i]);
			grid /= static_cast<float>(stats.grid.size());
			float histogram = 0.0f;
			for (size_t i = 0; i < stats.histogram.size(); ++i)
				histogram += std::fabs(stats.histogram[i] - m_previous.histogram[i]);
			histogram *= 0.5f;
			m_lastGrid = grid;
			m_lastHistogram = histogram;
			cut = (grid >= GRID_DIFFERENCE && histogram >= HISTOGRAM_DISTANCE) ||
				histogram >= HISTOGRAM_DISTANCE_ALONE;
		}
	}
	m_previous = stats;
	m_previousActive = stats.activePicture;
	m_havePrevious = true;
	return cut;
}
