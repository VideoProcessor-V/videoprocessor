// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraConsumerBinding.h"

#include <cctype>

namespace CameraLink
{
	CameraConsumerBinding::CameraConsumerBinding(CameraConsumer consumer, bool enabled) : m_consumer(consumer)
	{
		CameraModel::Consumer mc = CameraModel::Consumer::Debander;
		if (ToModelConsumer(consumer, &mc))
			m_policy = CameraModel::PolicyFor(mc);
		m_policy.enabled = enabled;
		m_defineName = "CAMERA_MODEL_";
		for (const char* p = Name(consumer) + 7; *p; ++p)   // skip "camera "
			m_defineName += *p == ' ' || *p == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
	}

	BindOutcome CameraConsumerBinding::Update(uint64_t sourceFrame, const CameraModel::CameraMotion* record,
		const CameraLabelsView& labels)
	{
		++m_stats.updates;
		// Off (or never enabled): the weight is 0, nothing is bound, no define: byte-identical to not having the camera.
		if (!m_policy.enabled)
		{
			m_state.Update(m_policy, nullptr);
			m_binding = CameraBinding();
			return BindOutcome::Off;
		}
		const bool repeat = m_haveFrame && sourceFrame == m_lastSourceFrame;
		if (!repeat)
		{
			m_state.Update(m_policy, record);   // the rate limit advances once per source frame
			m_haveFrame = true;
			m_lastSourceFrame = sourceFrame;
		}
		else
			++m_stats.repeats;
		CameraBinding b;
		b.domain = DomainMap();
		if (!record || !record->valid)
		{
			++m_stats.noRecord;
			m_binding = b;
			return BindOutcome::NoRecord;
		}
		if (!labels.available || !labels.tex)
		{
			++m_stats.noLabels;   // P8 absent: today's values, visible in the camera status
			m_binding = b;
			return BindOutcome::NoLabels;
		}
		b.constants = CameraModel::MakeAccessorConstants(m_policy, m_state, record);
		if (b.constants.enabled == 0)
		{
			m_binding = CameraBinding();
			++m_stats.noRecord;
			return BindOutcome::NoRecord;
		}
		b.tex = labels.tex;
		b.defineActive = true;
		b.defineName = m_defineName;
		b.labelsFloats[0] = labels.gridW;
		b.labelsFloats[1] = labels.gridH;
		m_binding = b;
		++m_stats.applied;
		return repeat ? BindOutcome::RepeatedFrame : BindOutcome::Applied;
	}
}
