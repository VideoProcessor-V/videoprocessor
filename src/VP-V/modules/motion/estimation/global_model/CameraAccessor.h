// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model (Plan 19): the consumer-side accessor. Pure, header-only, C++14, no Windows
// headers. A consumer owns one BlendState, calls Update() once per source frame with the frame's
// CameraMotion record, then calls FieldVector() once per block. With the default Policy
// (enabled = false) FieldVector returns exactly the caller's input floats: byte-identical to not
// having the model.
//
// Shader mirror: keep the branch order of FieldVector exactly; it uses only float-friendly
// operations (no recursion, no containers) so that it ports 1:1 to HLSL.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "CameraMotion.h"

namespace CameraModel
{
	struct Policy
	{
		double threshold = 0.7;      // confidence at/above which the model may supply a vector
		double floor = 0.5;          // hard fallback: below it the weight is 0 immediately
		double blendWidth = 0.1;     // smoothstep ramp width above threshold (weight 0 -> 1)
		double hysteresis = 0.05;    // once in use, the model stays until confidence < threshold - hysteresis
		double rateLimit = 0.25;     // maximum weight change per Update (per source frame)
		bool enabled = false;        // internal test switch; false = measured/hold value exactly
		bool preferModelWhenAgrees = false;
	};

	enum class Choice { Measured, Model, Blend, Hold };

	inline bool ModelUsable(const Policy& policy, const CameraMotion* record)
	{
		return policy.enabled && record != nullptr && record->valid && record->blocks != nullptr;
	}

	class BlendState
	{
	public:
		double Weight() const { return m_weight; }
		void Reset() { m_weight = 0.0; }

		// Once per source frame. Returns the new weight.
		double Update(const Policy& policy, const CameraMotion* record)
		{
			if (!policy.enabled)
			{
				m_weight = 0.0;
				return 0.0;
			}
			double target = 0.0;
			if (record == nullptr || !record->valid || ForcesZero(record->reason))
				target = 0.0;
			else if (record->confidence < policy.floor)
			{
				// Hard fallback: immediately, no rate limit.
				m_weight = 0.0;
				return 0.0;
			}
			else
			{
				const double threshold = m_weight > 0.0 ? policy.threshold - policy.hysteresis : policy.threshold;
				const double width = policy.blendWidth > 0.0 ? policy.blendWidth : 1e-9;
				double t = (record->confidence - threshold) / width;
				t = (std::min)(1.0, (std::max)(0.0, t));
				target = t * t * (3.0 - 2.0 * t);
			}
			const double step = (std::max)(0.0, policy.rateLimit);
			const double delta = target - m_weight;
			if (delta > step) m_weight += step;
			else if (delta < -step) m_weight -= step;
			else m_weight = target;
			m_weight = (std::min)(1.0, (std::max)(0.0, m_weight));
			return m_weight;
		}

	private:
		static bool ForcesZero(Reason reason)
		{
			return reason == Reason::Cut || reason == Reason::Flash || reason == Reason::DeadlineMissed ||
				reason == Reason::NoInput || reason == Reason::GenerationReset || reason == Reason::Disabled;
		}

		double m_weight = 0.0;
	};

	// One block's vector. (mx, my) is the caller's measured vector when measuredTrusted, otherwise
	// the caller's own fallback/hold value. (ox, oy) receives the result.
	inline Choice FieldVector(const Policy& policy, const BlendState& state, const CameraMotion* record, int bx, int by,
		bool measuredTrusted, float mx, float my, float& ox, float& oy)
	{
		const double w = state.Weight();
		bool haveModel = ModelUsable(policy, record) && w > 0.0;
		size_t index = 0;
		const BlockData* b = nullptr;
		if (haveModel)
		{
			b = record->blocks.get();
			if (bx < 0 || by < 0 || bx >= b->gridW || by >= b->gridH) haveModel = false;
			else
			{
				index = static_cast<size_t>(by) * static_cast<size_t>(b->gridW) + static_cast<size_t>(bx);
				const size_t n = static_cast<size_t>(b->gridW) * static_cast<size_t>(b->gridH);
				if (b->label.size() < n || b->modelX.size() < n || b->modelY.size() < n) haveModel = false;
			}
		}
		// Step 0: today's behaviour.
		if (!haveModel)
		{
			ox = mx;
			oy = my;
			return measuredTrusted ? Choice::Measured : Choice::Hold;
		}
		const bool camera = b->label[index] == static_cast<uint8_t>(Label::Camera);
		const float modelX = b->modelX[index], modelY = b->modelY[index];
		const float wf = static_cast<float>(w);
		if (measuredTrusted)
		{
			// Step 1: the measured vector stays unless the model agrees within noise and is preferred.
			if (policy.preferModelWhenAgrees && camera && b->resX.size() > index && b->resY.size() > index)
			{
				const float rx = b->resX[index], ry = b->resY[index];
				const float res = std::sqrt(rx * rx + ry * ry);
				const float mag = std::sqrt(modelX * modelX + modelY * modelY);
				// The fit's own agreement threshold (stored in the record): max(abs, rel * |model|).
				const float relLimit = b->relThreshold * mag;
				const float limit = relLimit > b->absThresholdPx ? relLimit : b->absThresholdPx;
				if (res <= limit)
				{
					if (wf >= 1.0f)
					{
						ox = modelX;
						oy = modelY;
						return Choice::Model;
					}
					ox = (1.0f - wf) * mx + wf * modelX;
					oy = (1.0f - wf) * my + wf * modelY;
					return Choice::Blend;
				}
			}
			ox = mx;
			oy = my;
			return Choice::Measured;
		}
		// Step 2: an untrusted Camera block takes the model vector.
		if (camera)
		{
			if (wf >= 1.0f)
			{
				ox = modelX;
				oy = modelY;
				return Choice::Model;
			}
			ox = (1.0f - wf) * mx + wf * modelX;
			oy = (1.0f - wf) * my + wf * modelY;
			return Choice::Blend;
		}
		// Step 3: today's fallback.
		ox = mx;
		oy = my;
		return Choice::Hold;
	}
}
