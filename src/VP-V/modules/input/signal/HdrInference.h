#pragma once

// Owner decision 2026-10-17 (newlldv repurposed): BT.2020 10/12-bit without DRM InfoFrame is treated as PQ/BT.2020
// (static metadata from [lldv.*], scene levels from VP peak detect). Real SDR sent as BT.2020 survives via a
// picture-statistics veto returning to SDR at a cut after SDR evidence in two shots (or after ~20 s without one).
// Pure policy (no renderer, capture or UI state) so the CPU tests own it.

#include <AnalysisLumaSource.h>
#include <VP-V/modules/input/source_analysis/events/shared/CutFadeJudge.h>
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
	// Bug hunt 37 S1: PQ epoch suspended across an HDMI re-sync; resumes immediately within resumeWindowMs.
	Suspended,
};

const char* ToString(HdrInferenceState state);

// Explicit owner settings permitting inference: Rec.709 hdr_colorspace or non-BT.2020 containerOverride forbid it.
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
	// DV input fixes 2026-10-18: explicit LLDV indication (Dolby VSIF or DV tunnel hand-off) lets 8-bit qualify.
	bool lldvIndicated = false;
	bool dvTunnelHandedOff = false;
	// Explicit owner settings (e.g. hdr_colorspace REC709) that forbid HDR10 inference.
	bool explicitSettingsForbid = false;
	uint64_t signalKey = 0;             // signal contract identity; change starts a new epoch
};

struct HdrInferenceEligibility
{
	bool eligible = false;
	const char* reason = "";
};

// Precedence: explicit settings > profile > newlldv inference. This decides
// only whether the EOTF may be inferred; explicit metadata settings still win
// for the fields they set (the caller applies them).
HdrInferenceEligibility EvaluateHdrInferenceEligibility(const HdrInferenceSignal& signal);

struct HdrInferenceOptions
{
	uint32_t confirmMs = 1500;          // stabilisation before inferring PQ
	bool sdrVeto = true;                // [general] newlldv_sdr_veto
	uint32_t shotEvidenceMs = 500;      // SDR-evidence time that marks a shot
	uint32_t shotsRequired = 2;         // shots with evidence before the veto arms
	uint32_t noCutFallbackMs = 20000;   // armed with no cut: switch anyway
	uint32_t maxSampleGapMs = 250;      // longest interval one sample may cover
	// F-8 either/or ([vpv] hdr_inference, HdrInferenceChoice.h): true = Bill's original newlldv
	// heuristic - BT.2020 + SDR EOTF + no HDR InfoFrame, confirmed after confirmMs (his 1500 ms),
	// no bit-depth / DV-tunnel / explicit-settings gates, no signal epochs, no SDR veto. The
	// Original pathway selects it. false (default) = VP's inference.
	bool originalHeuristic = false;
	// Bug hunt 37 S1: how long a PQ epoch survives a capture-signal gap (Suspended). A time
	// constant of the HDMI re-lock, not a rate. 0 = end at once (the behaviour before the fix).
	// Not used by the original heuristic (Bill's has no epochs).
	uint32_t resumeWindowMs = 10000;
};

// Bill's original newlldv candidate rule (upstream UpdateNewLldvCandidate): the capture state is
// valid, BT.2020, SDR EOTF and carries no HDR InfoFrame. Exactly upstream: the HDR modes do NOT
// gate the candidate; they gate its USE (his isHDFuryLLDV = confirmed && IsNewLldvModeSelected),
// see HdrInferenceChoice::ModesPermit.
HdrInferenceEligibility EvaluateOriginalNewLldvEligibility(const HdrInferenceSignal& signal);

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
	Suspended,   // bug hunt 37 S1: PQ epoch held across a signal gap (not PQ meanwhile)
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
	bool WantsPictureStatistics() const { return m_state == HdrInferenceState::InferredPq && VetoOn(); }
	uint64_t Epoch() const { return m_epoch; }
	uint32_t ShotsWithEvidence() const { return m_shotsWithEvidence; }
	bool VetoArmed() const { return m_vetoArmed; }
	uint32_t EpochFlips() const { return m_epochFlips; }
	// The current epoch began by resuming a suspended PQ epoch (bug hunt 37 S1), for the log.
	bool ResumedAfterGap() const { return m_resumedAfterGap; }
	const char* Reason() const { return m_reason; }

private:
	// The SDR veto is VP's; Bill's original heuristic has none.
	bool VetoOn() const { return m_options.sdrVeto && !m_options.originalHeuristic; }
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
	uint64_t m_suspendedSinceMs = 0;
	bool m_resumedAfterGap = false;
};

// Picture statistics for the veto (sparse limited 10-bit luma grid): SDR evidence when active
// picture has >= 0.2% samples >= 880 or p99.9 >= 870 (SDR peak white = 940; PQ 3000 nits ~ 827).
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

using HdrVetoSceneReaderFn = bool (*)(uint64_t sequence, bool& cutOut, const char** whyOut);
struct HdrVetoCutDiag
{
	uint64_t sharedHits = 0, fallbackCount = 0;
	const char* lastFallbackReason = nullptr;
	bool usedFallback = false;
};

// Shot boundaries between successive analysed pictures: consumes shared U7 when sealed for `sequence`,
// with visible GridHistCut + entering/leaving-black fallback when unkeyed or not yet sealed.
class HdrVetoCutDetector
{
public:
	using SceneReaderFn = HdrVetoSceneReaderFn;
	void SetSceneReader(SceneReaderFn reader) { m_sceneReader = reader; }
	bool Observe(const HdrVetoFrameStats& stats);
	bool Observe(const HdrVetoFrameStats& stats, uint64_t sequence);
	bool Observe(const HdrVetoFrameStats& stats, uint64_t sequence, SceneReaderFn reader, HdrVetoCutDiag* diagOut,
		const char* unkeyedReason = nullptr);
	void Reset();

	static constexpr float GRID_DIFFERENCE = VpUnit::CutFadeJudge::kGridDifference;
	static constexpr float HISTOGRAM_DISTANCE = VpUnit::CutFadeJudge::kHistogramDistance;
	static constexpr float HISTOGRAM_DISTANCE_ALONE = VpUnit::CutFadeJudge::kHistogramDistanceAlone;

	float LastGridDifference() const { return m_lastGrid; }
	float LastHistogramDistance() const { return m_lastHistogram; }
	uint64_t SharedCutHits() const { return m_sharedCutHits; }
	uint64_t FallbackCutCount() const { return m_fallbackCutCount; }
	const char* LastFallbackReason() const { return m_lastFallbackWhy; }
	bool UsedFallbackOnLastObserve() const { return m_lastUsedFallback; }

private:
	bool m_havePrevious = false, m_previousActive = false, m_lastUsedFallback = false;
	HdrVetoFrameStats m_previous;
	float m_lastGrid = 0.0f;
	float m_lastHistogram = 0.0f;
	SceneReaderFn m_sceneReader = nullptr;
	uint64_t m_previousSequence = 0, m_pendingSequence = 0, m_sharedCutHits = 0, m_fallbackCutCount = 0;
	const char* m_pendingUnkeyedReason = nullptr;
	const char* m_lastFallbackWhy = nullptr;
};
