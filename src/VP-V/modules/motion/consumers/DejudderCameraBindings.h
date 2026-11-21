// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// K9-3 (C6, B4): smoothed P9 camera bindings for the four motion-consumer stages in fixed ladder order:
//   1. Hiccup repair (gap-fill phase displacement across dropped frames);
//   2. R-A motion roll-off (fast-pan strength fall-off from smoothed camera speed);
//   3. De-judder phase warp (per-block phase vector via CameraPhase::AtPhase in log-scale / shortest-arc angle);
//   4. R-B camera/object split (object_strength separating camera motion from moving objects).
//
// Policy: strictest preset (CameraPhase::StrictPolicy: threshold 0.85, floor 0.70, blendWidth 0.05, rateLimit 0.20,
// identical to CameraModel::PolicyFor(CameraModel::Consumer::DejudderWarp)).
// Default OFF: when `enabled == false` (default, and forced off in the Original pathway), ICameraLink is never
// queried, every stage returns its uncompensated input byte-identically, and no status line or fallback is emitted.
// Absent / late / degenerate P9: falls back cleanly to uncompensated measured values with visible late counters
// and status line (never silent). Short-queue causal P7 is recorded visibly ("camera: causal (queue short)").
//
// Pure, deterministic, header-only, C++14, no Windows / GPU headers.
#pragma once

#include <VP-V/core/Ramp.h>
#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraLink.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraPhaseInterp.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraSmoother.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace VpMotion
{
	enum class DejudderCameraStage : uint8_t
	{
		Hiccup = 0,
		RollOffRA = 1,
		DejudderWarp = 2,
		ObjectSplitRB = 3
	};

	struct DejudderCameraConfig
	{
		bool enabled = false;           // camera_use_dejudder (default OFF; Original pathway forces OFF)
		bool hiccup = true;             // stage 1: hiccup repair binding (when enabled)
		bool rolloffRA = true;          // stage 2: R-A motion roll-off binding (when enabled)
		bool dejudderWarp = true;       // stage 3: de-judder phase warp binding (when enabled)
		bool objectSplitRB = true;      // stage 4: R-B camera/object split binding (when enabled)
		// R-A roll-off parameters (source px per frame; matches dejudder_roll_lo / hi / floor).
		double rollLo = 16.0;
		double rollHi = 26.0;
		double rollFloor = 0.25;
		// R-B object strength (0..1; 1.0 = unsplit, < 1 attenuates object motion relative to camera).
		double objectStrength = 1.0;
	};

	struct DejudderCameraStats
	{
		uint64_t queries = 0;
		uint64_t smoothedFrames = 0;    // frames served by P9 look-ahead smoothed model
		uint64_t causalFrames = 0;      // frames served by causal P7 (queue short)
		uint64_t lateFrames = 0;        // frames where camera was wanted but absent, invalid, degenerate or below floor
		uint64_t hiccupApplied = 0;
		uint64_t rolloffApplied = 0;
		uint64_t warpApplied = 0;
		uint64_t objectSplitApplied = 0;
		double lastWeight = 0.0;
		bool lastSmoothed = false;
		bool lastCausalFallback = false;
		bool lastLate = false;
	};

	// Prepared per-frame camera phase context, built once from ICameraLink::SmoothedRecord(seq) and reused
	// across the four ordered stages (one motion source; never starts the matcher).
	struct DejudderCameraFrame
	{
		bool active = false;            // usable P9/P7 model with blend weight > 0 and valid PhaseField
		bool wanted = false;            // config.enabled was true
		bool smoothed = false;          // true = P9 look-ahead smoothed, false = causal P7
		bool late = false;              // wanted, but record absent / unusable / degenerate / below floor
		double weight = 0.0;            // strict-policy rate-limited blend weight in [0, 1]
		double phase = 0.0;             // evaluated phase t in [-1, 2]
		double cameraSpeedPx = 0.0;     // characteristic camera displacement magnitude (source px/pair)
		CameraPhase::PhaseField field;  // precomputed similarity + residual evaluator at `phase`
		std::shared_ptr<const CameraModel::CameraMotion> record;
	};

	namespace detail
	{
		// Characteristic per-pair speed (source px) of a camera model: RMS displacement across centre + 4 corners.
		inline double ModelCharacteristicSpeed(const CameraModel::Params& p, const CameraModel::Rect& pic)
		{
			if (pic.Empty() || p.type == CameraModel::ModelType::None)
				return 0.0;
			const double hw = 0.5 * pic.Width(), hh = 0.5 * pic.Height();
			const double px[5] = { 0.0, -hw, hw, hw, -hw };
			const double py[5] = { 0.0, -hh, -hh, hh, hh };
			double sumSq = 0.0;
			int ok = 0;
			for (int i = 0; i < 5; ++i)
			{
				double dx = 0.0, dy = 0.0;
				if (CameraPhase::detail::ModelDisp(p, px[i], py[i], dx, dy))
				{
					sumSq += dx * dx + dy * dy;
					++ok;
				}
			}
			return ok > 0 ? std::sqrt(sumSq / static_cast<double>(ok)) : 0.0;
		}

		// Smoothstep roll-off scale in [rollFloor, 1.0]: 1 below rollLo, rollFloor above rollHi.
		inline double RollOffScale(double speedPx, double rollLo, double rollHi, double rollFloor)
		{
			const double floorClamped = RampMath::Clamp01NanToZero(rollFloor);
			if (!(rollHi > rollLo) || !(speedPx > rollLo))
				return 1.0;
			const double t = RampMath::Clamp01NanToZero((speedPx - rollLo) / (rollHi - rollLo));
			const double s = t * t * (3.0 - 2.0 * t);
			return 1.0 - s * (1.0 - floorClamped);
		}
	}

	// Render-thread state for K9-3 smoothed de-judder bindings (embedded in DejudderPolicyState, default OFF).
	struct DejudderCameraState
	{
		DejudderCameraConfig config;
		CameraModel::BlendState blend;
		DejudderCameraStats stats;
		std::vector<DejudderCameraStage> lastStageOrder;   // diagnostic trace of the stages executed on the last frame

		void Reset()
		{
			blend.Reset();
			stats = DejudderCameraStats();
			lastStageOrder.clear();
		}

		// Acquires P9 (or causal P7 fallback) once for frame `seq` at phase `phase` under the strictest policy.
		// When config.enabled is false, does NOT query `link` and returns an inactive frame with zero side effects.
		DejudderCameraFrame Prepare(const CameraLink::ICameraLink* link, uint64_t seq, double phase)
		{
			DejudderCameraFrame f;
			f.wanted = config.enabled;
			f.phase = phase;
			lastStageOrder.clear();
			if (!config.enabled)
				return f;

			++stats.queries;
			const CameraModel::Policy policy = CameraPhase::StrictPolicy(true);
			bool isSmoothed = false;
			std::shared_ptr<const CameraModel::CameraMotion> rec = link ? link->SmoothedRecord(seq, &isSmoothed) : nullptr;
			const double w = blend.Update(policy, rec.get());
			stats.lastWeight = w;

			if (!rec || !rec->valid || !CameraModel::ReasonUsable(rec->reason) || !(w > 0.0))
			{
				f.late = true;
				stats.lastLate = true;
				stats.lastSmoothed = false;
				stats.lastCausalFallback = false;
				++stats.lateFrames;
				return f;
			}

			const CameraModel::Params& active = CameraLink::ActiveParams(*rec);
			const CameraPhase::PhaseField pf = CameraPhase::AtPhase(active, rec->picture, phase);
			if (!pf.Ok())
			{
				// Degenerate model (NaN / horizon / extreme scale): refuse visibly, never warp.
				blend.Reset();
				f.late = true;
				stats.lastWeight = 0.0;
				stats.lastLate = true;
				stats.lastSmoothed = false;
				stats.lastCausalFallback = false;
				++stats.lateFrames;
				return f;
			}

			f.active = true;
			f.smoothed = isSmoothed;
			f.late = false;
			f.weight = w;
			f.cameraSpeedPx = detail::ModelCharacteristicSpeed(active, rec->picture);
			f.field = pf;
			f.record = std::move(rec);

			stats.lastLate = false;
			stats.lastSmoothed = isSmoothed;
			stats.lastCausalFallback = !isSmoothed;
			if (isSmoothed)
				++stats.smoothedFrames;
			else
				++stats.causalFrames;
			return f;
		}

		// Stage 1: Hiccup repair (gap fill across dropped frames). Blends measured gap vector with
		// phase-interpolated camera displacement at picture-centred point (cx, cy).
		void ApplyHiccup(const DejudderCameraFrame& f, double cx, double cy, float measuredVx, float measuredVy,
			float& outVx, float& outVy)
		{
			outVx = measuredVx;
			outVy = measuredVy;
			if (!f.active || !config.hiccup)
				return;
			double cdx = 0.0, cdy = 0.0;
			f.field.Displacement(cx, cy, cdx, cdy);
			const double w = f.weight;
			outVx = static_cast<float>((1.0 - w) * measuredVx + w * cdx);
			outVy = static_cast<float>((1.0 - w) * measuredVy + w * cdy);
			++stats.hiccupApplied;
			lastStageOrder.push_back(DejudderCameraStage::Hiccup);
		}

		// Stage 2: R-A motion roll-off. Computes roll-off scale from camera speed blended with measured speed.
		double ApplyRollOffRA(const DejudderCameraFrame& f, double measuredSpeedPx)
		{
			if (!f.active || !config.rolloffRA)
				return detail::RollOffScale(measuredSpeedPx, config.rollLo, config.rollHi, config.rollFloor);
			const double speed = (1.0 - f.weight) * measuredSpeedPx + f.weight * f.cameraSpeedPx;
			++stats.rolloffApplied;
			lastStageOrder.push_back(DejudderCameraStage::RollOffRA);
			return detail::RollOffScale(speed, config.rollLo, config.rollHi, config.rollFloor);
		}

		// Stage 3: De-judder phase warp grid (CPU mirror). Evaluates phase-interpolated camera vectors on the
		// block grid and blends camera-labelled blocks with measured vectors; object / overlay / untrusted blocks
		// keep measured vectors unchanged. Off / late -> exact copy of measured vectors.
		void ApplyDejudderPhaseGrid(const DejudderCameraFrame& f, int gridW, int gridH, double blockSize,
			const std::vector<float>& measVx, const std::vector<float>& measVy, const std::vector<uint8_t>& trusted,
			std::vector<float>& outVx, std::vector<float>& outVy)
		{
			outVx = measVx;
			outVy = measVy;
			const size_t n = static_cast<size_t>(gridW > 0 ? gridW : 0) * static_cast<size_t>(gridH > 0 ? gridH : 0);
			if (!f.active || !config.dejudderWarp || !f.record || measVx.size() < n || measVy.size() < n || trusted.size() < n)
				return;
			const CameraModel::BlockData* bd = f.record->blocks.get();
			const bool hasLabels = bd && bd->gridW == gridW && bd->gridH == gridH && bd->label.size() >= n;
			const double w = f.weight;
			for (int by = 0; by < gridH; ++by)
			{
				for (int bx = 0; bx < gridW; ++bx)
				{
					const size_t i = static_cast<size_t>(by) * static_cast<size_t>(gridW) + static_cast<size_t>(bx);
					if (hasLabels && bd->label[i] != static_cast<uint8_t>(CameraModel::Label::Camera))
						continue;   // object / overlay / unknown: never overwritten with camera warp
					if (!hasLabels && trusted[i] == 0)
						continue;
					double cx = 0.0, cy = 0.0, cdx = 0.0, cdy = 0.0;
					CameraModel::BlockCentre(bx, by, blockSize, f.record->picture, &cx, &cy);
					f.field.Displacement(cx, cy, cdx, cdy);
					outVx[i] = static_cast<float>((1.0 - w) * measVx[i] + w * cdx);
					outVy[i] = static_cast<float>((1.0 - w) * measVy[i] + w * cdy);
				}
			}
			++stats.warpApplied;
			lastStageOrder.push_back(DejudderCameraStage::DejudderWarp);
		}

		// Stage 4: R-B camera/object split. At objectStrength < 1, objects moving relative to the camera move by
		// cam + objectStrength * (meas - cam); at objectStrength >= 1 or off/late, returns measured unchanged.
		void ApplyObjectSplitRB(const DejudderCameraFrame& f, double cx, double cy, float measuredVx, float measuredVy,
			float& outVx, float& outVy)
		{
			outVx = measuredVx;
			outVy = measuredVy;
			const double os = RampMath::Clamp01NanToZero(config.objectStrength);
			if (!f.active || !config.objectSplitRB || os >= 1.0)
				return;
			double cdx = 0.0, cdy = 0.0;
			f.field.Displacement(cx, cy, cdx, cdy);
			const double splitVx = cdx + os * (static_cast<double>(measuredVx) - cdx);
			const double splitVy = cdy + os * (static_cast<double>(measuredVy) - cdy);
			const double w = f.weight;
			outVx = static_cast<float>((1.0 - w) * measuredVx + w * splitVx);
			outVy = static_cast<float>((1.0 - w) * measuredVy + w * splitVy);
			++stats.objectSplitApplied;
			lastStageOrder.push_back(DejudderCameraStage::ObjectSplitRB);
		}

		// Visible status line when camera_use_dejudder is active (empty when off -> byte-identical status).
		std::string StatusSuffix() const
		{
			if (!config.enabled)
				return std::string();
			if (stats.lastLate && stats.lateFrames > 0)
			{
				char b[128];
				std::snprintf(b, sizeof(b), ", de-judder: camera late (%llu frames): uncompensated path used",
					static_cast<unsigned long long>(stats.lateFrames));
				return b;
			}
			if (stats.lastCausalFallback)
				return std::string(", ") + CameraLink::kCausalShortQueueText;
			if (stats.lastSmoothed)
			{
				char b[96];
				std::snprintf(b, sizeof(b), ", camera P9 weight %.2f", stats.lastWeight);
				return b;
			}
			return std::string();
		}
	};
}
