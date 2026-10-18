#pragma once

// Owner decision 2026-10-17 (newlldv repurposed): with [general] newlldv on, a
// BT.2020, 10/12-bit signal that carries no HDR (DRM) InfoFrame is treated as
// PQ / BT.2020. This covers LLDV and ordinary HDR10 behind a device that drops
// only the DRM InfoFrame (HDFury Integral 2 with its HDR toggle off: the AVI
// InfoFrame still reports BT.2020). Static metadata then comes from the active
// [lldv.*] profile, and VP's peak detection handles the scene levels.
//
// Real SDR sent as BT.2020 must survive: a picture-statistics veto returns the
// signal to SDR, only at a cut, after SDR evidence in two shots, at most once
// per signal epoch. With no cut for about 20 s after the veto arms, it switches
// without one.
//
// Pure policy (no renderer, capture or UI state) so the CPU tests own it.

#include <AnalysisLumaSource.h>
#include <ColorSpace.h>
#include <EOTF.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class HdrInferenceState
{
	Off,          // newlldv off
	NotEligible,  // the signal does not qualify (see the eligibility reason)
	Waiting,      // qualifying signal, stabilising before PQ is inferred
	InferredPq,   // PQ / BT.2020 is inferred for this epoch
	VetoedSdr,    // the SDR veto returned this epoch to SDR (final for the epoch)
};

const char* ToString(HdrInferenceState state);

// Explicit owner settings that allow the inference: an hdr_colorspace of
// Rec.709 contradicts a BT.2020 HDR reading, and so (bug hunt 3 C6) does a
// container colorspace override to anything but BT.2020 - the owner then says
// the signal is not a BT.2020 container, which the inference requires.
// containerOverride: the container override (UNKNOWN = follow the input).
inline bool HdrSettingsPermitInference(bool hdrColorspaceRec709, ColorSpace containerOverride)
{
	return !hdrColorspaceRec709 &&
		(containerOverride == ColorSpace::UNKNOWN || containerOverride == ColorSpace::BT_2020);
}

struct HdrInferenceSignal
{
	bool enabled = false;               // [general] newlldv
	bool valid = false;                 // capture state is valid
	ColorSpace colorspace = ColorSpace::UNKNOWN;  // raw capture colorimetry
	EOTF eotf = EOTF::UNKNOWN;          // raw capture EOTF
	bool hdrInfoFrame = false;          // the source sent static HDR metadata
	int bitDepth = 0;                   // source bit depth; 0 = unknown
	bool dolbyVisionTunnel = false;     // the DV tunnel owns this signal
	// Explicit owner settings that force a non-HDR10 reading. hdr_colorspace
	// REC709 declares the source's primaries as Rec.709.
	bool explicitSettingsForbid = false;
	// Identity of the signal contract (mode, rate, encoding). A change starts
	// a new epoch even while the signal stays eligible.
	uint64_t signalKey = 0;
};

struct HdrInferenceEligibility
{
	bool eligible = false;
	const char* reason = "";
};

// Precedence: explicit settings > profile > newlldv inference. This decides
// only whether the EOTF may be inferred; explicit metadata settings still win
// for the fields they set (the caller applies them).
HdrInferenceEligibility EvaluateHdrInferenceEligibility(
	const HdrInferenceSignal& signal);

struct HdrInferenceOptions
{
	uint32_t confirmMs = 1500;          // stabilisation before inferring PQ
	bool sdrVeto = true;                // [general] newlldv_sdr_veto
	uint32_t shotEvidenceMs = 500;      // SDR-evidence time that marks a shot
	uint32_t shotsRequired = 2;         // shots with evidence before the veto arms
	uint32_t noCutFallbackMs = 20000;   // armed with no cut: switch anyway
	uint32_t maxSampleGapMs = 250;      // longest interval one sample may cover
};

// One analysed picture, in the order the capture delivered it.
struct HdrPictureObservation
{
	bool measured = false;      // statistics were computed (false: skipped)
	bool activePicture = false; // enough non-black picture to judge
	bool sdrEvidence = false;   // the frame is SDR-conclusive
	bool cut = false;           // a shot boundary precedes this frame
	double fractionAt880 = 0.0; // diagnostics
	uint16_t p999 = 0;
};

enum class HdrInferenceEvent
{
	None,
	CandidateSeen,
	Inferred,
	CandidateCleared,
	Ended,
	NewEpoch,
	ShotEvidence,
	VetoArmed,
	VetoAtCut,
	VetoWithoutCut,
};

const char* ToString(HdrInferenceEvent event);

struct HdrInferenceUpdate
{
	HdrInferenceEvent event = HdrInferenceEvent::None;
	bool effectiveChanged = false;  // IsInferredPq() changed
};

class HdrInferenceTracker
{
public:
	void SetOptions(const HdrInferenceOptions& options) { m_options = options; }
	const HdrInferenceOptions& Options() const { return m_options; }

	HdrInferenceUpdate OnSignal(const HdrInferenceSignal& signal, uint64_t nowMs);
	HdrInferenceUpdate OnPicture(const HdrPictureObservation& picture, uint64_t nowMs);
	HdrInferenceUpdate OnTick(uint64_t nowMs);
	void Reset();

	HdrInferenceState State() const { return m_state; }
	bool IsInferredPq() const { return m_state == HdrInferenceState::InferredPq; }
	bool IsCandidate() const
	{
		return m_state == HdrInferenceState::Waiting ||
			m_state == HdrInferenceState::InferredPq ||
			m_state == HdrInferenceState::VetoedSdr;
	}
	// The capture path computes picture statistics only while this is true.
	bool WantsPictureStatistics() const
	{
		return m_state == HdrInferenceState::InferredPq && m_options.sdrVeto;
	}
	uint64_t Epoch() const { return m_epoch; }
	uint32_t ShotsWithEvidence() const { return m_shotsWithEvidence; }
	bool VetoArmed() const { return m_vetoArmed; }
	uint32_t EpochFlips() const { return m_epochFlips; }
	const char* Reason() const { return m_reason; }

private:
	HdrInferenceUpdate Flip(HdrInferenceEvent event);
	void StartEpoch(uint64_t nowMs, uint64_t signalKey);
	void ResetShots();

	HdrInferenceOptions m_options;
	HdrInferenceState m_state = HdrInferenceState::Off;
	const char* m_reason = "newlldv off";
	uint64_t m_epoch = 0;
	uint64_t m_signalKey = 0;
	uint64_t m_candidateSinceMs = 0;
	// Veto bookkeeping for the current shot and epoch.
	uint64_t m_lastPictureMs = 0;
	bool m_havePicture = false;
	uint64_t m_shotEvidenceMs = 0;
	bool m_shotCounted = false;
	uint32_t m_shotsWithEvidence = 0;
	bool m_vetoArmed = false;
	uint64_t m_vetoArmedMs = 0;
	uint32_t m_epochFlips = 0;
};

// Picture statistics for the veto, from a sparse luma grid in the limited
// 10-bit code domain (AnalysisLumaSource::Sample emits that domain for every
// format). A frame is SDR evidence when, over the active (letterbox-cropped)
// picture, >= 0.2% of samples are at code >= 880 or the 99.9th percentile is
// >= 870. Real PQ material rarely reaches those codes (870 is about 3000 nits);
// SDR peak white sits at 940.
struct HdrVetoThresholds
{
	uint16_t highCode = 880;
	double highFraction = 0.002;
	uint16_t p999Code = 870;
	uint16_t blackCode = 88;            // row is active when >= 2% exceed this
	double activeRowFraction = 0.02;
	double minimumActiveRows = 0.10;    // less active picture: not judged
};

struct HdrVetoFrameStats
{
	static constexpr int GRID_COLUMNS = 16;
	static constexpr int GRID_ROWS = 9;
	static constexpr int HISTOGRAM_BINS = 16;

	bool measured = false;
	bool activePicture = false;
	bool sdrEvidence = false;
	uint32_t samples = 0;
	uint32_t activeSamples = 0;
	uint32_t atOrAboveHigh = 0;
	double fractionAtHigh = 0.0;
	uint16_t p999 = 0;
	// Cut-detection signature over the whole frame.
	std::array<float, GRID_COLUMNS * GRID_ROWS> grid{};
	std::array<float, HISTOGRAM_BINS> histogram{};
};

// grid: columns x rows samples, row-major, limited 10-bit codes.
HdrVetoFrameStats MeasureHdrVetoGrid(const uint16_t* grid, int columns, int rows,
	const HdrVetoThresholds& thresholds = HdrVetoThresholds());

// Samples the analysis source on a columns x rows grid (cell centres).
bool SampleHdrVetoGrid(const AnalysisLumaSource& source, int columns, int rows,
	std::vector<uint16_t>& grid);

// Shot boundaries between successive analysed pictures (about 20 Hz). Tuned on
// the D:\hfr clips sampled at 10-20 Hz: within-shot motion stays below a 0.15
// histogram distance; cuts between different shots exceed it with a large
// grid difference, or exceed 0.35 alone. Entering or leaving black is a cut.
class HdrVetoCutDetector
{
public:
	bool Observe(const HdrVetoFrameStats& stats);
	void Reset() { m_havePrevious = false; }

	static constexpr float GRID_DIFFERENCE = 24.0f;
	static constexpr float HISTOGRAM_DISTANCE = 0.15f;
	static constexpr float HISTOGRAM_DISTANCE_ALONE = 0.35f;

	float LastGridDifference() const { return m_lastGrid; }
	float LastHistogramDistance() const { return m_lastHistogram; }

private:
	bool m_havePrevious = false;
	bool m_previousActive = false;
	HdrVetoFrameStats m_previous;
	float m_lastGrid = 0.0f;
	float m_lastHistogram = 0.0f;
};
