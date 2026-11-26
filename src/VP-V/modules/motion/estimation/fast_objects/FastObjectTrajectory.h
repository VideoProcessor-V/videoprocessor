// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Stage B: camera-stabilized multi-frame window mapping (Cramer-Rao covariance propagation),
// exact Hungarian GNN association, bounce/impulse two-leg stitching, BIC 4-model trajectory
// classifier (LinearGlide / Ballistic / Impulse / Clutter), 2.5D perspective depth kinematics,
// motion-blur tangent alignment & online shutter estimator, 64x36 clutter-hotspot auto-mask,
// and lightweight session ball appearance prior.
// Pure C++14/17, deterministic, zero static/thread_local mutable state.
#pragma once

#include <cstdint>

#include "FastObjectTypes.h"

namespace FastObjects
{
	// One stabilized observation mapped into frame t's world-plane coordinates (source px, top-left origin).
	struct WindowSample
	{
		double tauSec = 0.0;           // Time offset relative to target frame t (seconds; 0 at frame t)
		double dtStepSec = 0.0;        // Inter-frame duration at this sample (seconds, for shutter estimate)
		double x = 0.0;                // Stabilized centroid X in frame t source px
		double y = 0.0;                // Stabilized centroid Y in frame t source px
		double covXX = 1.0;            // 2x2 measurement covariance R_k in source px^2
		double covXY = 0.0;
		double covYY = 1.0;
		double radiusPx = 1.0;         // Deconvolved minor-axis radius r_est (source px)
		double streakLenPx = 0.0;      // Deconvolved blur streak length L_est (source px)
		double streakDirX = 1.0;       // Stabilized principal streak unit vector X
		double streakDirY = 0.0;       // Stabilized principal streak unit vector Y
		double intSnr = 4.0;           // Integrated SNR
		bool overlapsMask = false;     // P8 Label::Overlay or CE-WATER waterProb >= 0.5
		bool nearLargeObject = false;  // Within 2*r + blockSize of a large P8 Label::Object
	};

	struct TrajectoryClassification
	{
		TrajectoryType type = TrajectoryType::Unknown;
		double p0x = 0.0, p0y = 0.0;   // Fitted position at tau = 0 (source px)
		double vxSec = 0.0, vySec = 0.0; // Instantaneous velocity at tau = 0 (source px/s)
		double axSec2 = 0.0, aySec2 = 0.0; // Acceleration (source px/s^2)
		double nextDispX = 0.0, nextDispY = 0.0; // Predicted displacement p(pairSec) - p(0) (source px)
		double chi2 = 0.0;
		double bic = 0.0;
		double bicMargin = 0.0;        // Runner-up valid BIC minus winning BIC
		int modelDof = 0;

		bool hasImpulse = false;
		double impulseTauSec = 0.0;    // Vertex time offset relative to frame t (seconds)
		double impulseX = 0.0, impulseY = 0.0;
		double vMinusX = 0.0, vMinusY = 0.0;
		double vPlusX = 0.0, vPlusY = 0.0;

		bool usedPerspectiveDepth = false;
		double radiusAtZero = 0.0;
		double radiusRatePerSec = 0.0;

		double blurAlignFactor = 1.0;  // g_blur in (0, 1]
		double shutterEst = 0.0;       // Online median shutter fraction tau_s in [0, 1]
		bool blurStretched = false;
		int velocityReversals = 0;
		bool rejectedStationary = false;
		bool rejectedOscillatory = false;
		bool rejectedIncoherent = false;
		bool rejectedMaskOverlap = false;
	};

	// Pure stateless BIC 4-model classifier over N stabilized samples in frame t's coordinates.
	// Evaluates LinearGlide (d=4 CV / d=5 drag), Ballistic (d=6), Impulse (d=7 / d=8 shared gravity),
	// 2.5D perspective depth, motion-blur tangent alignment, and explicit Clutter null/oscillatory/chi2/mask gates.
	TrajectoryClassification ClassifyTrajectoryWindow(
		const Config& cfg,
		const WindowSample* samples,
		int count,
		double pairSeconds,
		const CameraModel::Rect& picture);

	// Exact O(N^3) Hungarian minimum-cost assignment on an nRows x nCols matrix (max 32x32, zero heap allocs).
	// Costs >= forbiddenCost are treated as disallowed edges. OutRowToCol[r] receives assigned col or -1.
	void SolveHungarian32(
		int nRows,
		int nCols,
		const double cost[kMaxTracksHardCap][kMaxBlobs],
		double forbiddenCost,
		int outRowToCol[kMaxTracksHardCap]);

	// One frame's Stage A detections + camera model stored in the 13-frame look-ahead/back ring.
	struct FrameObservation
	{
		uint64_t frameIndex = 0;
		double timestampSec = 0.0;
		double dtFromPrevSec = 0.0;
		CameraModel::Params camPrevToCur;
		bool camPrevUsable = false;
		double camSigmaPx = 0.0;
		CameraModel::Rect picture;
		BlobCandidate blobs[kMaxBlobs];
		uint32_t blobCount = 0;
		bool causalMode = false;
		bool chromaUsed = false;
		bool valid = false;
	};

	// Lightweight exponentially smoothed session ball appearance prior (soft priority only).
	struct SessionBallPrior
	{
		double polarityScore = 0.0;    // In [-1, 1]
		double meanCb = 0.0;
		double meanCr = 0.0;
		double meanRadiusPx = 0.0;
		double minRadiusPx = 0.0;
		double maxRadiusPx = 0.0;
		bool hasChroma = false;
		uint32_t updates = 0;

		void Reset()
		{
			polarityScore = 0.0;
			meanCb = 0.0;
			meanCr = 0.0;
			meanRadiusPx = 0.0;
			minRadiusPx = 0.0;
			maxRadiusPx = 0.0;
			hasChroma = false;
			updates = 0;
		}
	};

	// Stateful Stage B multi-hypothesis tracker owning the 13-frame observation ring,
	// active/coasting tracks, 64x36 clutter-hotspot auto-mask, and session appearance prior.
	class TrajectoryTracker
	{
	public:
		TrajectoryTracker();

		// Clears tracks, ring, and clutter hotspots (called on scene cut / flash / resync).
		// Preserves SessionBallPrior across cuts within the same source.
		void ResetScene();

		// Full reset including SessionBallPrior (called on source / resolution / channel change).
		void ResetSession();

		// Pushes a new frame's Stage A blobs into the ring buffer (monotonic frameIndex).
		// Automatically advances sequential association + bounce stitching up to this frame.
		void PushFrame(const Config& cfg, const FrameObservation& obs);

		// Resolves and classifies all tracks at `targetFrameIndex` using the full available window
		// [target - W_b, target + W_a] in the ring. Populates `outSet` (prior to Stage B.5 refinement).
		// If targetFrameIndex is the latest frame in the ring (W_a == 0), flags causalMode visibly.
		bool ResolveAt(const Config& cfg, uint64_t targetFrameIndex, FastObjectTrackSet& outSet);

		// Queries whether any active or coasting track currently exists (for Dormant scheduler).
		bool HasActiveOrCoastingTracks() const;

		// Diagnostic accessors (read-only).
		const SessionBallPrior& Prior() const { return sessionPrior_; }
		float HotspotHeatAt(float x, float y, const CameraModel::Rect& picture) const;

	private:
		struct TrackPoint
		{
			uint64_t frameIndex = 0;
			double timestampSec = 0.0;
			double dtFromPrevSec = 0.0;
			BlobCandidate blob;
			double nis = 0.0;
			bool crossingAmbiguous = false;
			bool valid = false;
		};

		struct TrackRecord
		{
			uint32_t id = 0;
			bool active = false;
			bool confirmed = false;
			bool priorMatched = false;
			TrajectoryType lastType = TrajectoryType::Unknown;
			double lastNis = 0.0;
			double coastElapsedSec = 0.0;
			uint32_t coastFrames = 0;
			int8_t polarity = 0;
			double emaFlux = 0.0;
			double emaChromaFlux = 0.0;
			double emaRadiusPx = 0.0;
			double emaCb = 0.0;
			double emaCr = 0.0;
			bool hasChroma = false;
			double lastBgLuma = 0.0;
			// Circular/shifted history of up to kWindowRingCapacity associated observations.
			TrackPoint history[kWindowRingCapacity];
			int historyCount = 0;
		};

		int FindRingSlot(uint64_t frameIndex) const;
		bool ComposeHomographyBetween(uint64_t fromFrame, uint64_t toFrame, CameraModel::Params& outH, double& outAccumCamVarPx2) const;
		void DecayHotspots(double dtSec, double decaySec);
		void AddHotspotHeat(float x, float y, const CameraModel::Rect& picture, float amount);
		bool IsHotspotSuppressed(float x, float y, const CameraModel::Rect& picture) const;
		void UpdateSessionPrior(const TrackRecord& trk);
		double SessionPriorBonus(const Config& cfg, const BlobCandidate& b) const;
		void ConfirmTrackIncrementally(const Config& cfg, int latestSlot, TrackRecord& trk);
		void AssociateLatestFrame(const Config& cfg, int latestSlot);
		void AttemptImpulseStitching(const Config& cfg, int latestSlot);
		void PruneExpiredTracks(const Config& cfg);

		FrameObservation ring_[kWindowRingCapacity];
		int ringCount_ = 0;
		uint64_t latestFrameIndex_ = 0;
		bool hasLatestFrame_ = false;

		TrackRecord tracks_[kMaxTracksHardCap];
		uint32_t nextTrackId_ = 1;

		float hotspotHeat_[kHotspotGridCells];
		SessionBallPrior sessionPrior_;
		uint32_t lastHotspotSuppressed_ = 0;
	};
}
