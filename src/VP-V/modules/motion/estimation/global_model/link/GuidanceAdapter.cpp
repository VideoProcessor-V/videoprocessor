// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GuidanceAdapter.h"

#include <cstdio>

namespace CameraLink
{
	CameraGuidanceAdapter::CameraGuidanceAdapter(CameraLinkCore& core, const GuidanceSettings& settings, const GuidanceAdapterHooks& hooks)
		: m_core(core), m_hooks(hooks), m_settings(settings), m_sched(*core.Link(), settings), m_hook(m_sched, nullptr, *core.Link()),
		m_job(m_sched, *core.Link())
	{
	}

	CameraGuidanceAdapter::~CameraGuidanceAdapter()
	{
		Detach();
	}

	void CameraGuidanceAdapter::Attach(VpMotion::IVectorEngine* engine, std::unique_ptr<IGuidanceCountsSource> counts)
	{
		Detach();
		if (!engine)
			return;
		m_engine = engine;
		m_counts = std::move(counts);
		m_hook.SetCountsSource(m_counts.get());
		m_flowMayBeResident.store(engine->FlowIsCpuResident(), std::memory_order_relaxed);
		m_sched.SetContextProvider(MakeContextProvider(engine, *m_core.Link(), m_hooks.p4, m_hooks.onRenderThread,
			[this]() { return m_flowMayBeResident.load(std::memory_order_relaxed); }));
		m_sched.SetSlipCheck([engine](uint64_t recordSeq, uint64_t pairSeq) { return engine->PairSpansSlip(recordSeq, pairSeq); });
		m_job.SetNextPairHint(m_hooks.nextPair);
		Reconcile();
	}

	void CameraGuidanceAdapter::Detach(bool engineValid)
	{
		// Render thread, renderMutex held (GuidanceAdapter.h LOCK RULE): SetFitDoneObserver below waits for the worker's pump.
		if (m_hooked && m_engine && engineValid)
			m_engine->SetPreEstimateHook(nullptr);   // FIRST: the engine must not call a hook that is going away
		m_hooked = false;
		m_core.SetFitDoneObserver(nullptr);          // waits for an observer call in flight on the camera worker
		m_engine = nullptr;
		m_job.Reset();
		m_sched.SetContextProvider(nullptr);
		m_sched.SetSlipCheck(nullptr);
		m_hook.SetCountsSource(nullptr);
		m_counts.reset();
		m_bench.Close();
	}

	void CameraGuidanceAdapter::Reconcile()
	{
		const bool want = m_engine && m_settings.switches.Any();
		if (want && !m_hooked)
		{
			m_bench.Open();   // VP_MOTION_BENCH (closed when the variable is unset)
			m_sched.SetBench(m_bench.IsOpen() ? &m_bench : nullptr);
			m_core.SetFitDoneObserver([this](uint64_t fitSeq) { m_job.OnFitDone(fitSeq); m_job.Pump(); });
			m_engine->SetPreEstimateHook(&m_hook);
			m_hooked = true;
		}
		else if (!want && m_hooked)
		{
			m_engine->SetPreEstimateHook(nullptr);
			m_hooked = false;
			m_core.SetFitDoneObserver(nullptr);
			m_job.Reset();
			m_sched.SetBench(nullptr);
			m_bench.Close();
		}
	}

	void CameraGuidanceAdapter::SetSettings(const GuidanceSettings& settings)
	{
		m_settings = settings;
		m_sched.SetSettings(settings);
		Reconcile();
	}

	uint8_t CameraGuidanceAdapter::RebuildGraph(double sourcePeriodSeconds, bool deterministic)
	{
		const uint8_t d = m_core.LagMeter().Lag(sourcePeriodSeconds, deterministic);
		m_sched.SetLag(d);
		m_job.Reset();
		return d;
	}

	void CameraGuidanceAdapter::OnDiscontinuity()
	{
		m_sched.ResetForDiscontinuity();   // bumps the epoch first: a lane pass mid-compute cannot store afterwards
		m_job.Reset();
		++m_discontinuities;
	}

	void CameraGuidanceAdapter::OnFrame()
	{
		if (!m_hooked)
			return;
		if (m_engine)
			m_flowMayBeResident.store(m_engine->FlowIsCpuResident(), std::memory_order_relaxed);
		m_hook.Poll();
		m_job.Pump();
	}

	void CameraGuidanceAdapter::CollectStatus(std::vector<std::string>& lines) const
	{
		const std::vector<std::string> s = m_sched.StatusLines();
		lines.insert(lines.end(), s.begin(), s.end());
		if (!m_hooked)
			return;
		const GuideLaneJob::Stats j = m_job.GetStats();
		char b[200];
		std::snprintf(b, sizeof(b), "camera guidance lane: %llu stored, %llu retried, %llu discarded, %llu dropped",
			(unsigned long long)j.published, (unsigned long long)j.retried, (unsigned long long)j.discarded, (unsigned long long)j.dropped);
		lines.push_back(b);
	}
}
