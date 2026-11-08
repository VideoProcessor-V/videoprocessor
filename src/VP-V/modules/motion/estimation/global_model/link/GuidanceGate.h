// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-3): the per-pair refusal gate of the guided matcher, as ONE pure function. BuildGuide() (lane and live)
// calls it, so the two can never disagree about whether a pair is guided. Reasons, in the order they are judged:
//   AllOff         every measurement switch is off: nothing is bound, today's matcher runs, byte-identical
//   NoRecord       P7(N-d) is not there ("camera record late")
//   Slip           the pair spans a drop / repeat / resync, or the stamp of the record is ancient
//   NotUsable      !Usable(record): the D-K4 verdict set (Ambiguous, InverseMismatch, Degenerate, TooFewSamples,
//                  LowConfidence) or an invalid record ("no dominant motion")
//   LowConfidence  confidence < the plan floor (0.7); 0.85 when the ONLY switch on is the static gate (E4 from the model is
//                  stricter; with other switches on, the plan applies the 0.85 to the static step itself)
// The remaining refusals (Governor, NoPlan) are not about the record: the scheduler and the plan builder raise them.
// Every refusal is counted per reason by the scheduler and shown in the status (never log-only).
// Pure, header-only, C++14, deterministic.
#pragma once

#include "CameraUsable.h"

#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelMeasurement.h>

#include <algorithm>
#include <cstdint>

namespace CameraLink
{
	enum class Refusal : uint8_t { None, AllOff, NoRecord, Slip, NotUsable, LowConfidence, Governor, NoPlan, Count };

	inline const char* Text(Refusal r)
	{
		switch (r)
		{
		case Refusal::None: return "guided";
		case Refusal::AllOff: return "all camera guidance switches off";
		case Refusal::NoRecord: return "camera record late";
		case Refusal::Slip: return "pair spans a slip";
		case Refusal::NotUsable: return "no dominant motion";
		case Refusal::LowConfidence: return "camera confidence too low";
		case Refusal::Governor: return "escalations high";
		case Refusal::NoPlan: return "plan empty";
		default: return "?";
		}
	}

	// True when the static gate is the only switch on.
	inline bool OnlyStaticGate(const CameraModel::MeasurementSwitches& sw)
	{
		if (!sw.staticGate)
			return false;
		CameraModel::MeasurementSwitches rest = sw;
		rest.staticGate = false;
		return !rest.Any();
	}

	// The record confidence below which the pair is refused.
	inline double ConfidenceFloor(const CameraModel::MeasurementSwitches& sw, const CameraModel::MeasurementConfig& cfg)
	{
		return OnlyStaticGate(sw) ? (std::max)(cfg.minConfidence, cfg.staticConfidence) : cfg.minConfidence;
	}

	// The gate. `record` is P7(N-d) (null = late). `spansSlip` / `ancientStamp` come from the host's pair context.
	inline Refusal GuidanceRefusal(const CameraModel::MeasurementSwitches& sw, const CameraModel::MeasurementConfig& cfg,
		const CameraModel::CameraMotion* record, bool spansSlip, bool ancientStamp)
	{
		if (!sw.Any())
			return Refusal::AllOff;
		if (!record)
			return Refusal::NoRecord;
		if (spansSlip || ancientStamp)
			return Refusal::Slip;
		if (!Usable(*record))
			return Refusal::NotUsable;
		if (record->confidence < ConfidenceFloor(sw, cfg))
			return Refusal::LowConfidence;
		return Refusal::None;
	}
}
