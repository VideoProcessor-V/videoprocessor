// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19, phase 2): the measurement plan (see GlobalModelMeasurement.h).
// Pure C++14, deterministic (fixed loop orders), no threads, no unordered containers, no pixel constants.
#include "GlobalModelMeasurement.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "GlobalModelGeometry.h"

namespace CameraModel
{
	namespace
	{
		// Why a previous record cannot drive a model-based saving; null = usable by reason.
		const char* UnusableReason(Reason r)
		{
			switch (r)
			{
			case Reason::Cut:
			case Reason::PostCutSeeded: return "after a cut";
			case Reason::Flash: return "after a flash";
			case Reason::GenerationReset: return "after a resync or re-base";
			case Reason::DeadlineMissed: return "camera record deadline missed";
			case Reason::NoInput:
			case Reason::Disabled: return "no camera record";
			default: return nullptr;
			}
		}

		Params ObjectParams(const ObjectModel& o)
		{
			Params p;
			p.type = o.type;
			for (int j = 0; j < 9; ++j) p.h[j] = o.h[j];
			return p;
		}

		const ObjectModel* ObjectOf(const CameraMotion& prev, const BlockData& b, size_t i)
		{
			if (b.objectId.size() <= i || b.objectId[i] == 0) return nullptr;
			for (const ObjectModel& o : prev.objects)
				if (o.id == b.objectId[i]) return &o;
			return nullptr;
		}

		void Recount(MeasurementPlan& plan)
		{
			plan.counters.narrowBlocks = plan.counters.fullBlocks = plan.counters.finerBlocks = plan.counters.excludedBlocks = 0;
			for (uint8_t h : plan.hint)
				switch (static_cast<BlockHint>(h))
				{
				case BlockHint::FullSearch: ++plan.counters.fullBlocks; break;
				case BlockHint::NarrowAroundPrediction: ++plan.counters.narrowBlocks; break;
				case BlockHint::FinerGrid: ++plan.counters.finerBlocks; break;
				case BlockHint::Excluded: ++plan.counters.excludedBlocks; break;
				}
		}
	}

	MeasurementPlan BuildMeasurementPlan(const MeasurementSwitches& sw, const MeasurementConfig& cfg, const MeasurementInput& in)
	{
		MeasurementPlan plan;
		if (!sw.Any()) return plan;
		plan.active = true;
		plan.guidedCandidate = sw.guidedCandidate;
		plan.e1Skip = sw.e1Skip;

		// ---- scene ----
		// A cut is only labelled SkipAcrossCut when the matcher really skips it (cutSkip on); with the switch off the
		// scene stays Normal so that a consumer of `scene` alone cannot change today's search.
		if (in.cut)
		{
			plan.seedFromNext = in.haveNextPair;
			if (sw.cutSkip) { plan.scene = SceneAction::SkipAcrossCut; plan.skipMatcher = true; }
		}
		else if (in.flash)
		{
			if (sw.flashBridge) { plan.scene = SceneAction::BridgeFlash; plan.skipMatcher = true; }
		}
		else if (in.fade && sw.fadeNormalised)
		{
			plan.scene = SceneAction::FadeNormalised;
			plan.fadeNormalised = true;
			plan.fadeGain = in.fadeGain;
			plan.fadeOffset = in.fadeOffset;
			plan.counters.fadeNormalisedUsed = true;
		}
		if (plan.skipMatcher)
		{
			plan.counters.matcherSkipped = true;
			return plan;
		}

		const bool wantBlocks = sw.narrowWindow || sw.temporalSeed || sw.guidedCandidate || sw.e1Skip || sw.tinyMoverRoi || sw.objects;
		const bool wantModel = sw.modelGlobal || sw.staticGate || wantBlocks;
		if (!wantModel) return plan;

		// ---- is the previous record usable? ----
		const CameraMotion* prev = in.prev;
		const char* why = nullptr;
		if (in.cut) why = "scene cut";
		else if (prev == nullptr) why = "no camera record";
		else if (!prev->valid) { why = UnusableReason(prev->reason); if (why == nullptr) why = "camera record not valid"; }
		else if (UnusableReason(prev->reason) != nullptr) why = UnusableReason(prev->reason);
		else if (prev->confidence < cfg.minConfidence) why = "low confidence";
		if (why != nullptr)
		{
			plan.fallbackReason = why;
			return plan;
		}

		const BlockData* b = prev->blocks.get();
		bool blocksOk = false;
		if (b != nullptr && b->gridW > 0 && b->gridH > 0)
		{
			const size_t n = static_cast<size_t>(b->gridW) * static_cast<size_t>(b->gridH);
			blocksOk = b->label.size() >= n && b->modelX.size() >= n && b->modelY.size() >= n;
		}
		const double k = (in.nextPairSeconds > 0.0 && prev->pairSeconds > 0.0) ? in.nextPairSeconds / prev->pairSeconds : 1.0;
		const double blockSize = b != nullptr && b->blockSize > 0 ? b->blockSize : 16.0;
		const double precision = in.precisionPx > 0.0 ? in.precisionPx : 0.25 * blockSize / 16.0;

		// ---- model as D1's global ----
		if (sw.modelGlobal)
		{
			double dx = 0, dy = 0, bx = 0, by = 0;
			Displacement(prev->params, 0.0, 0.0, &dx, &dy);
			bool ok = false;
			const Params inv = Invert(prev->params, &ok);
			if (ok) Displacement(inv, 0.0, 0.0, &bx, &by);
			else { bx = -dx; by = -dy; }
			plan.useModelGlobal = true;
			plan.globalX = k * dx;
			plan.globalY = k * dy;
			plan.globalBwdX = k * bx;
			plan.globalBwdY = k * by;
			plan.counters.modelGlobalUsed = true;
		}

		// ---- E4 from the model ----
		if (sw.staticGate && prev->confidence >= cfg.staticConfidence && prev->reason == Reason::Ok)
		{
			double hw = 0.5 * prev->picture.Width(), hh = 0.5 * prev->picture.Height();
			if (prev->picture.Empty() && b != nullptr) { hw = 0.5 * b->gridW * b->blockSize; hh = 0.5 * b->gridH * b->blockSize; }
			if (hw > 0 && hh > 0)
			{
				const double limit = cfg.staticSigmas * precision;
				bool still = true;
				for (int c = 0; c < 4 && still; ++c)
				{
					double dx = 0, dy = 0;
					Displacement(prev->params, (c & 1) ? hw : -hw, (c & 2) ? hh : -hh, &dx, &dy);
					if (std::hypot(k * dx, k * dy) > limit) still = false;
				}
				plan.staticScene = still;
				plan.counters.staticFromModel = still;
			}
		}

		if (!wantBlocks) return plan;
		if (!blocksOk)
		{
			plan.fallbackReason = prev->labelsOnGpu ? "labels on GPU only" : "no per-block record";
			return plan;
		}

		// ---- per-block data ----
		HintConfig hc;
		hc.minConfidence = cfg.minConfidence;
		hc.agreeSigmas = cfg.agreeSigmas;
		hc.narrowRadiusSigmas = cfg.narrowRadiusSigmas;
		hc.temporalSeed = sw.temporalSeed && !prev->bridged;
		hc.tinyMoverRoi = sw.tinyMoverRoi;
		hc.cutSkip = sw.cutSkip;
		hc.fadeNormalised = sw.fadeNormalised;
		hc.flashBridge = sw.flashBridge;
		const bool temporal = hc.temporalSeed && in.prevFwdX != nullptr && in.prevFwdY != nullptr;
		SearchHints hints = BuildHints(hc, prev, in.prevFwdX, in.prevFwdY, in.nvofX, in.nvofY, false, false, false,
			in.fadeGain, in.fadeOffset, precision, in.nextPairSeconds);
		const size_t n = static_cast<size_t>(b->gridW) * static_cast<size_t>(b->gridH);
		if (hints.hint.size() != n)
		{
			plan.fallbackReason = "no per-block record";
			return plan;
		}
		plan.gridW = b->gridW;
		plan.gridH = b->gridH;
		plan.blockSize = b->blockSize;
		plan.hint.swap(hints.hint);
		plan.seedX.swap(hints.seedX);
		plan.seedY.swap(hints.seedY);
		plan.rois.swap(hints.rois);
		plan.narrowRadiusPx = std::max(cfg.narrowMinRadiusPx, hints.narrowRadiusPx);
		plan.e1AgreePx = cfg.e1AgreePx;
		plan.eligible.assign(n, 0);
		plan.predX.assign(n, 0.0f);
		plan.predY.assign(n, 0.0f);

		const bool narrowOn = sw.narrowWindow || sw.temporalSeed;
		for (size_t i = 0; i < n; ++i)
		{
			const uint8_t label = b->label[i];
			const BlockHint h = static_cast<BlockHint>(plan.hint[i]);
			plan.predX[i] = static_cast<float>(k * b->modelX[i]);
			plan.predY[i] = static_cast<float>(k * b->modelY[i]);
			const ObjectModel* om = label == static_cast<uint8_t>(Label::Object) ? ObjectOf(*prev, *b, i) : nullptr;
			const bool fixedHint = h == BlockHint::Excluded || h == BlockHint::FinerGrid;
			if (!fixedHint)
			{
				if (label == static_cast<uint8_t>(Label::Camera)) plan.eligible[i] = 1;
				else if (om != nullptr && sw.objects)
				{
					double cx = 0, cy = 0, dx = 0, dy = 0;
					BlockCentre(static_cast<int>(i % static_cast<size_t>(b->gridW)), static_cast<int>(i / static_cast<size_t>(b->gridW)),
						b->blockSize, prev->picture, &cx, &cy);
					Displacement(ObjectParams(*om), cx, cy, &dx, &dy);
					plan.predX[i] = static_cast<float>(k * dx);
					plan.predY[i] = static_cast<float>(k * dy);
					plan.eligible[i] = 1;
				}
			}
			if (h == BlockHint::NarrowAroundPrediction)
			{
				if (!narrowOn || (om != nullptr && !sw.objects)) plan.hint[i] = static_cast<uint8_t>(BlockHint::FullSearch);
				else
				{
					if (om != nullptr) ++plan.counters.objectSeeded;
					if (temporal) ++plan.counters.temporalSeeded;
				}
			}
		}

		// ---- tiny-mover ROI budget (raster order, as produced) ----
		if (!plan.rois.empty())
		{
			const double budget = cfg.roiBudgetShare * static_cast<double>(n);
			double total = 0;
			for (const SearchHints::Roi& r : plan.rois) total += static_cast<double>(r.bx1 - r.bx0) * (r.by1 - r.by0);
			if (total > budget)
			{
				double running = 0;
				bool over = false;
				size_t kept = 0;
				for (size_t r = 0; r < plan.rois.size(); ++r)
				{
					const SearchHints::Roi& roi = plan.rois[r];
					running += static_cast<double>(roi.bx1 - roi.bx0) * (roi.by1 - roi.by0);
					if (running > budget) over = true;
					if (!over) { ++kept; continue; }
					for (int y = roi.by0; y < roi.by1; ++y)
						for (int x = roi.bx0; x < roi.bx1; ++x)
						{
							const size_t i = static_cast<size_t>(y) * static_cast<size_t>(b->gridW) + static_cast<size_t>(x);
							if (plan.hint[i] == static_cast<uint8_t>(BlockHint::FinerGrid))
							{
								plan.hint[i] = static_cast<uint8_t>(BlockHint::FullSearch);
								++plan.counters.roiOverBudget;
							}
						}
				}
				plan.rois.resize(kept);   // only the ROIs actually processed stay listed
				if (plan.fallbackReason[0] == '\0') plan.fallbackReason = "tiny-mover ROI budget exceeded";
			}
		}
		Recount(plan);
		return plan;
	}

	uint32_t ApplyVerification(MeasurementPlan& plan, const uint8_t* accepted)
	{
		if (!plan.active || accepted == nullptr || plan.hint.empty()) return 0;
		uint32_t count = 0;
		for (size_t i = 0; i < plan.hint.size(); ++i)
		{
			const bool guided = plan.hint[i] == static_cast<uint8_t>(BlockHint::NarrowAroundPrediction) ||
				(plan.e1Skip && i < plan.eligible.size() && plan.eligible[i] != 0);
			if (!guided || accepted[i] != 0) continue;
			plan.hint[i] = static_cast<uint8_t>(BlockHint::FullSearch);
			if (i < plan.eligible.size()) plan.eligible[i] = 0;
			++count;
		}
		plan.counters.escalated += count;
		Recount(plan);
		return count;
	}

	void MeasurementDiagnostics::Add(const MeasurementPlan& plan)
	{
		if (!plan.active) return;
		++frames;
		narrow += plan.counters.narrowBlocks;
		full += plan.counters.fullBlocks;
		finer += plan.counters.finerBlocks;
		escalated += plan.counters.escalated;
		roiOverBudget += plan.counters.roiOverBudget;
		if (plan.counters.modelGlobalUsed) ++modelGlobal;
		if (plan.counters.staticFromModel) ++staticFromModel;
		if (plan.counters.matcherSkipped) ++matcherSkipped;
		if (plan.counters.fadeNormalisedUsed) ++fadeNormalised;
		if (plan.fallbackReason != nullptr && plan.fallbackReason[0] != '\0') lastFallback = plan.fallbackReason;
	}

	std::string MeasurementDiagnostics::StatusLine() const
	{
		char buf[512];
		const double planned = static_cast<double>(narrow + full + finer);
		const double pct = planned > 0 ? 100.0 / planned : 0.0;
		std::snprintf(buf, sizeof(buf),
			"camera guidance: narrow %.0f%% (escalated %.1f%%), full %.0f%%, finer %.1f%%, D1 model %llu/%llu, static %llu, skipped pairs %llu, fade-normalised %llu",
			pct * static_cast<double>(narrow), pct * static_cast<double>(escalated), pct * static_cast<double>(full), pct * static_cast<double>(finer),
			static_cast<unsigned long long>(modelGlobal), static_cast<unsigned long long>(frames),
			static_cast<unsigned long long>(staticFromModel), static_cast<unsigned long long>(matcherSkipped),
			static_cast<unsigned long long>(fadeNormalised));
		std::string s(buf);
		if (roiOverBudget > 0) s += ", ROI over budget " + std::to_string(roiOverBudget);
		if (!lastFallback.empty()) s += ", last fallback: " + lastFallback;
		return s;
	}

	void PackGuideTexels(const MeasurementPlan& plan, std::vector<float>& guideRgba, std::vector<float>& code)
	{
		guideRgba.clear();
		code.clear();
		if (!plan.active || plan.hint.empty()) return;
		const size_t n = plan.hint.size();
		if (plan.seedX.size() < n || plan.seedY.size() < n || plan.predX.size() < n || plan.predY.size() < n || plan.eligible.size() < n) return;
		guideRgba.resize(n * 4);
		code.resize(n);
		for (size_t i = 0; i < n; ++i)
		{
			guideRgba[4 * i + 0] = plan.seedX[i];
			guideRgba[4 * i + 1] = plan.seedY[i];
			guideRgba[4 * i + 2] = plan.predX[i];
			guideRgba[4 * i + 3] = plan.predY[i];
			code[i] = static_cast<float>(plan.hint[i] + 4 * (plan.eligible[i] != 0 ? 1 : 0));
		}
	}
}
