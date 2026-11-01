// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-3, D-K4): the verdict gate. ONE usable set (CameraModel::ReasonUsable) shared by the accessor weight
// (BlendState::ForcesZero) and the measurement guidance gate; a test pins both to the same set. A record that is not
// usable is still PUBLISHED with its true reason (diagnostics); no consumer uses its model.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <cstdint>

namespace CameraLink
{
	constexpr double kUsableMinConfidence = 0.5;

	inline bool Usable(const CameraModel::CameraMotion& r)
	{
		return r.valid && r.confidence >= kUsableMinConfidence && CameraModel::ReasonUsable(r.reason);
	}
	inline bool Usable(const CameraModel::CameraMotion* r) { return r != nullptr && Usable(*r); }

	// Plain status wording for a record that is published but not used (null = usable).
	inline const char* NotUsedText(const CameraModel::CameraMotion& r)
	{
		if (Usable(r))
			return nullptr;
		switch (r.reason)
		{
		case CameraModel::Reason::Ambiguous: return "no dominant motion (ambiguous), model not used";
		case CameraModel::Reason::InverseMismatch: return "forward/backward mismatch, model not used";
		case CameraModel::Reason::Degenerate: return "degenerate model, not used";
		case CameraModel::Reason::TooFewSamples: return "too few samples, model not used";
		case CameraModel::Reason::LowConfidence: return "low confidence, model not used";
		default: break;
		}
		return "model not used";
	}
}
