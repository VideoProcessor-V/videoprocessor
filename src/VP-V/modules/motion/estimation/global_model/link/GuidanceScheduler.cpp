// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GuidanceScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace CameraLink
{
	namespace
	{
		constexpr size_t kLaneRing = 16;
		constexpr size_t kPendingRing = 16;
		constexpr size_t kAwaitingSealMax = 32;   // pairs guided before their on-time seal (the seal runs within a few frames)

		bool SameVec(const std::vector<float>& a, const std::vector<float>& b)
		{
			return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
		}

		uint32_t EligibleCount(const CameraModel::MeasurementPlan& p)
		{
			uint32_t n = 0;
			for (uint8_t e : p.eligible)
				n += e ? 1u : 0u;
			return n;
		}
	}

	GuideResult BuildGuide(const CameraModel::MeasurementSwitches& sw, const CameraModel::MeasurementConfig& cfg, const GuideInputs& in)
	{
		GuideResult r;
		// K4-3: the ONE refusal gate (GuidanceGate.h). Lane and live both pass through here.
		r.refusal = GuidanceRefusal(sw, cfg, in.record, in.ctx.spansSlip, in.ctx.ancientStamp);
		if (r.refusal == Refusal::AllOff || r.refusal == Refusal::NotOnTime)
			return r;
		// The record's own confidence and reason are reported for refused pairs too (bench rows, status).
		r.confidence = in.record->confidence;
		r.reason = CameraModel::ReasonTextOf(in.record->reason, in.record->verdictFlags);
		if (r.refusal != Refusal::None)
			return r;
		CameraModel::MeasurementInput mi;
		mi.prev = in.record;
		const CameraModel::BlockData* b = in.record->blocks.get();
		// prevFwd: never seeded from a junk pair (after a flash or a cut) and only when the grids agree.
		if (in.prevField && in.prevField->Valid() && b && in.prevField->gridW == b->gridW && in.prevField->gridH == b->gridH &&
			!in.ctx.flash && !in.ctx.cut)
		{
			mi.prevFwdX = in.prevField->fwdX.data();
			mi.prevFwdY = in.prevField->fwdY.data();
		}
		if (in.ctx.flowCpuResident && b && in.ctx.nvofX.size() == static_cast<size_t>(b->gridW) * b->gridH && in.ctx.nvofY.size() == in.ctx.nvofX.size())
		{
			mi.nvofX = in.ctx.nvofX.data();
			mi.nvofY = in.ctx.nvofY.data();
		}
		mi.cut = in.ctx.cut;
		mi.fade = in.ctx.fade;
		mi.flash = in.ctx.flash;
		mi.fadeGain = in.ctx.fadeGain;
		mi.fadeOffset = in.ctx.fadeOffset;
		mi.nextPairSeconds = in.ctx.nextPairSeconds;
		r.plan = CameraModel::BuildMeasurementPlan(sw, cfg, mi);
		// An "active" plan can still carry nothing to apply (the builder refused the model: fallbackReason says why).
		const bool nothing = r.plan.fallbackReason && r.plan.fallbackReason[0] != 0 && r.plan.hint.empty() &&
			!r.plan.skipMatcher && !r.plan.useModelGlobal && !r.plan.staticScene && !r.plan.fadeNormalised;
		if (!r.plan.Active() || nothing)
		{
			r.refusal = Refusal::NoPlan;
			return r;
		}
		r.frame = std::make_shared<const CameraModel::GuidanceFrame>(CameraModel::MakeGuidanceFrame(r.plan));
		r.refusal = Refusal::None;
		return r;
	}

	bool SameGuide(const CameraModel::GuidanceFrame& a, const CameraModel::GuidanceFrame& b)
	{
		return a.active == b.active && a.skipMatcher == b.skipMatcher && a.useModelGlobal == b.useModelGlobal &&
			std::memcmp(a.globalFwd, b.globalFwd, sizeof(a.globalFwd)) == 0 && std::memcmp(a.globalBwd, b.globalBwd, sizeof(a.globalBwd)) == 0 &&
			a.staticScene == b.staticScene && a.narrow == b.narrow && a.e1Skip == b.e1Skip && a.fadeNormalised == b.fadeNormalised &&
			a.gridW == b.gridW && a.gridH == b.gridH && a.blockSize == b.blockSize && a.narrowRadiusPx == b.narrowRadiusPx &&
			a.e1AgreePx == b.e1AgreePx && SameVec(a.guide, b.guide) && SameVec(a.code, b.code);
	}

	GuidanceOutcome ParseGuidanceCounts(const float c[4], uint32_t narrowBlocksPlanned, uint32_t e1EligibleBlocks)
	{
		GuidanceOutcome o;
		for (int i = 0; i < 2; ++i)
			if (c[i] >= 0.0f)
			{
				o.valid = true;
				o.narrowPlanned += narrowBlocksPlanned;
				o.narrowAccepted += static_cast<uint32_t>(c[i] + 0.5f);
			}
		for (int i = 2; i < 4; ++i)
			if (c[i] >= 0.0f)
			{
				o.valid = true;
				o.e1Eligible += e1EligibleBlocks;
				o.e1Accepted += static_cast<uint32_t>(c[i] + 0.5f);
			}
		const uint32_t nFail = o.narrowPlanned > o.narrowAccepted ? o.narrowPlanned - o.narrowAccepted : 0u;
		const uint32_t eFail = o.e1Eligible > o.e1Accepted ? o.e1Eligible - o.e1Accepted : 0u;
		o.escalated = nFail + eFail;
		return o;
	}

	uint32_t GuidanceScheduler::CfgKeyOf(const CameraModel::MeasurementConfig& c)
	{
		// FNV-1a over the bytes of each threshold (no padding is hashed: field by field).
		uint32_t h = 2166136261u;
		const double v[] = { c.minConfidence, c.staticConfidence, c.staticSigmas, c.agreeSigmas, c.narrowRadiusSigmas,
			c.narrowMinRadiusPx, c.e1AgreePx, c.roiBudgetShare };
		for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); ++i)
		{
			unsigned char b[sizeof(double)];
			std::memcpy(b, &v[i], sizeof(double));
			for (size_t k = 0; k < sizeof(double); ++k)
			{
				h ^= b[k];
				h *= 16777619u;
			}
		}
		return h;
	}

	GuidanceScheduler::GuidanceScheduler(ICameraLink& link, const GuidanceSettings& settings)
		: m_link(link), m_settings(settings), m_governor(settings.governor), m_lane(kLaneRing), m_pending(kPendingRing), m_asked(kLaneRing, ~uint64_t(0))
	{
	}

	void GuidanceScheduler::SetSettings(const GuidanceSettings& s)
	{
		{
			// New thresholds / switches: a lane result built under the old ones must not be used (it is also keyed, this drops it now).
			std::lock_guard<std::mutex> lock(m_mutex);
			for (Stored& st : m_lane)
				st = Stored();
			++m_epoch;
		}
		m_settings = s;
		m_governor = GuidanceGovernor(s.governor);
		RefreshSnapshot();
	}

	void GuidanceScheduler::RefreshSnapshot()
	{
		const CameraModel::MeasurementSwitches governed = m_governor.Apply(m_settings.switches);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_snapSwitches = governed;
		m_snapConfig = m_settings.config;
		m_snapContext = m_context;
	}

	void GuidanceScheduler::ResetForDiscontinuity()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (Stored& s : m_lane)
				s = Stored();
			std::fill(m_asked.begin(), m_asked.end(), ~uint64_t(0));   // the numbering may restart: nothing counts as asked
			++m_epoch;   // a lane pass that is mid-compute right now must not store its result
		}
		for (Stored& s : m_pending)
			s = Stored();
		m_awaitingSeal.clear();   // the numbering may restart: an old seq's verdict slot is not this pair's
		m_governor.Reset();
		m_hold.Reset();
		m_narrowIdleRemaining = 0;
		RefreshSnapshot();
	}

	GuideResult GuidanceScheduler::Compute(uint64_t pairSeq, const std::shared_ptr<const CameraModel::CameraMotion>& rec, OnTimeMiss miss)
	{
		// d = 0: camera(N) on time or nothing (no late path). prevFwd stays absent: the only other field at hand is N's own.
		GuideInputs in;
		in.record = rec.get();
		in.ctx.recordSeq = pairSeq;
		if (rec && m_context && !m_context(pairSeq, in.ctx))
		{
			// P4(N) not published when the matcher of N runs: camera(N) is not usable in time.
			in.record = nullptr;
			miss = OnTimeMiss::NotPublished;
		}
		const CameraModel::MeasurementSwitches sw = m_governor.Apply(m_settings.switches);
		GuideResult r = BuildGuide(sw, m_settings.config, in);
		if (r.refusal == Refusal::AllOff && m_settings.switches.Any())
			r.refusal = Refusal::Governor;   // only narrow / E1 were on and the governor turned them off
		if (r.refusal == Refusal::NotOnTime)
			r.miss = miss == OnTimeMiss::None ? OnTimeMiss::NotPublished : miss;
		return r;
	}

	bool GuidanceScheduler::LaneWanted() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_snapSwitches.Any();
	}

	GuidanceScheduler::LaneOutcome GuidanceScheduler::TryPublishLane(uint64_t pairSeq)
	{
		// Everything the worker thread reads is a snapshot taken under the mutex (same governed switches as the render thread).
		CameraModel::MeasurementSwitches sw;
		CameraModel::MeasurementConfig cfg;
		std::function<bool(uint64_t, PairContext&)> context;
		uint64_t epoch;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			sw = m_snapSwitches;
			cfg = m_snapConfig;
			context = m_snapContext;
			epoch = m_epoch;
			if (!sw.Any())
				return LaneOutcome::Off;
			if (m_asked[pairSeq % m_asked.size()] == pairSeq)
			{
				m_laneDiscarded.fetch_add(1, std::memory_order_relaxed);   // the render computed it live first: a late lane result is discarded, never merged
				return LaneOutcome::Discarded;
			}
		}
		// Worker thread: the publish-side checks only (the stamp / seal checks read render-thread state; Guide re-reads camera(N)
		// in full and uses this result only for the very same record).
		OnTimeMiss why = OnTimeMiss::None;
		const std::shared_ptr<const CameraModel::CameraMotion> rec = m_link.OnTimeRecord(pairSeq, OnTimeCheck::PublishOnly, why);
		if (!rec)
			return LaneOutcome::NotReady;   // camera(N) not on time: nothing stored (the render counts the cause)
		GuideInputs in;
		in.record = rec.get();
		in.ctx.recordSeq = pairSeq;
		if (context && !context(pairSeq, in.ctx))
			return LaneOutcome::NotReady;   // P4(N) is not published yet
		GuideResult r = BuildGuide(sw, cfg, in);
		if (r.refusal == Refusal::NotOnTime)
			return LaneOutcome::NotReady;
		r.fromLane = true;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_epoch != epoch)
			return LaneOutcome::Discarded;   // a discontinuity / new settings arrived mid-compute: this result is from the old numbering
		if (m_asked[pairSeq % m_asked.size()] == pairSeq)
		{
			m_laneDiscarded.fetch_add(1, std::memory_order_relaxed);
			return LaneOutcome::Discarded;
		}
		Stored& s = m_lane[pairSeq % m_lane.size()];
		s.seq = pairSeq;
		s.key = KeyOf(sw);
		s.cfgKey = CfgKeyOf(cfg);
		s.rec = rec;
		s.result = std::move(r);
		m_lanePublished.fetch_add(1, std::memory_order_relaxed);
		return LaneOutcome::Published;
	}

	void GuidanceScheduler::StoreLaneForTest(uint64_t pairSeq, const GuideResult& result, const CameraModel::MeasurementConfig& builtUnder)
	{
		OnTimeMiss why = OnTimeMiss::None;
		const std::shared_ptr<const CameraModel::CameraMotion> rec = m_link.OnTimeRecord(pairSeq, OnTimeCheck::Full, why);
		std::lock_guard<std::mutex> lock(m_mutex);
		Stored& s = m_lane[pairSeq % m_lane.size()];
		s.seq = pairSeq;
		s.key = KeyOf(m_governor.Apply(m_settings.switches));
		s.cfgKey = CfgKeyOf(builtUnder);
		s.rec = rec;
		s.result = result;
		s.result.fromLane = true;
	}

	GuideResult GuidanceScheduler::Guide(uint64_t pairSeq, double pairSeconds, uint64_t fromSeq)
	{
		// The cool-down runs on MEASURED pair time for every pair asked about, guided or not (a narrow-only configuration
		// has no guided pair to judge, and must still recover).
		if (m_governor.Tripped())
		{
			m_governor.Elapse(pairSeconds);
			RefreshSnapshot();
		}
		const uint32_t key = KeyOf(m_governor.Apply(m_settings.switches));
		const uint32_t cfgKey = CfgKeyOf(m_settings.config);
		// THE d = 0 decision, once per pair: camera(N) ready in time, in full, for exactly this pair - or unguided.
		OnTimeMiss miss = OnTimeMiss::None;
		std::shared_ptr<const CameraModel::CameraMotion> recNow;
		if (m_settings.switches.Any())
		{
			recNow = m_link.OnTimeRecord(pairSeq, OnTimeCheck::Full, miss);
			if (recNow && !SameCameraPair(*recNow, fromSeq, pairSeconds))
			{
				recNow.reset();
				miss = OnTimeMiss::NotThePair;   // a gap / hiccup fill pair: camera(N) measured another pair
			}
		}
		GuideResult out;
		bool haveLane = false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_asked[pairSeq % m_asked.size()] = pairSeq;
			Stored& s = m_lane[pairSeq % m_lane.size()];
			// A lane result built under other switches / thresholds, or from another record, is never used.
			if (s.seq == pairSeq && s.key == key && s.cfgKey == cfgKey && recNow && s.rec == recNow)
			{
				out = s.result;
				haveLane = true;
			}
			s = Stored();
		}
		if (haveLane && out.frame && m_slipCheck && m_slipCheck(recNow->pairFrom, pairSeq))
		{
			// The slip appeared after the lane built the guide: the same refusal the live fill would give.
			out.frame.reset();
			out.refusal = Refusal::Slip;
		}
		if (haveLane)
			++m_counters.fromLane;
		else
		{
			out = Compute(pairSeq, recNow, miss);
			out.fromLane = false;
			if (out.frame)
				++m_counters.fromLive;
		}
		++m_counters.refusals[size_t(out.refusal)];
		if (out.refusal == Refusal::NotOnTime)
			++m_counters.notOnTime[size_t(out.miss)];
		m_hold.Observe(out.refusal, pairSeconds);
		if (m_narrowIdleRemaining > 0)
			m_narrowIdleRemaining -= pairSeconds > 0 ? pairSeconds : kGuidanceRefusalHoldSeconds;
		if (m_bench && !out.frame)
		{
			BenchRow row;
			row.pair = pairSeq;
			row.seq = recNow ? pairSeq : 0;
			row.switchMask = SwitchMask(m_settings.switches);
			row.d = 0;
			row.confidence = out.confidence;
			row.reason = Text(out.refusal);
			row.governorState = m_governor.Tripped() ? 1 : 0;
			m_bench->Write(row);
		}
		if (out.frame)
		{
			++m_counters.guided;
			Stored& p = m_pending[pairSeq % m_pending.size()];
			p.seq = pairSeq;
			p.rec = recNow;
			p.result = out;
			p.gpuMs = -1;
			// Guided before N's on-time seal (the usual order): the seal's verdict is read back by PollSeals (R when rejected).
			if (m_link.OnTimeSealVerdict(pairSeq) == SealVerdict::Pending)
			{
				if (m_awaitingSeal.size() >= kAwaitingSealMax)
					m_awaitingSeal.erase(m_awaitingSeal.begin());
				m_awaitingSeal.push_back(pairSeq);
			}
		}
		return out;
	}

	void GuidanceScheduler::PollSeals()
	{
		size_t keep = 0;
		for (size_t i = 0; i < m_awaitingSeal.size(); ++i)
		{
			const SealVerdict v = m_link.OnTimeSealVerdict(m_awaitingSeal[i]);
			if (v == SealVerdict::Rejected)
				++m_counters.rejectedAtSeal;
			else if (v == SealVerdict::Pending)
				m_awaitingSeal[keep++] = m_awaitingSeal[i];
		}
		m_awaitingSeal.resize(keep);
	}

	void GuidanceScheduler::NoteGpuMs(uint64_t pairSeq, double ms)
	{
		Stored& p = m_pending[pairSeq % m_pending.size()];
		if (p.seq == pairSeq && p.result.frame)
			p.gpuMs = ms;
	}

	std::vector<FallbackCatalog::Decision> GuidanceScheduler::CollectDecisions() const
	{
		GuidanceStatusFacts f;
		f.switchesOn = m_settings.switches.Any();
		f.governorTripped = m_governor.Tripped();
		f.refusedNow = m_hold.Engaged();
		f.lastRefusal = m_hold.Last();
		f.narrowIdleNow = m_narrowIdleRemaining > 0;
		f.rejectedAtSeal = m_counters.rejectedAtSeal;
		return BuildGuidanceDecisions(f);
	}

	void GuidanceScheduler::OnEstimateOutcome(uint64_t pairSeq, const EstimateOutcome& o)
	{
		if (o.gpuMs >= 0)
			NoteGpuMs(pairSeq, o.gpuMs);
		if (o.narrowDroppedEngine)
		{
			++m_counters.narrowIdleEngine;
			m_narrowIdleRemaining = kGuidanceRefusalHoldSeconds;
		}
		if (o.rejected && o.rejected[0] != 0)
		{
			++m_counters.rejectedByMatcher;
			m_rejectedText = o.rejected;
		}
		if (o.gpuNotApplied && o.gpuNotApplied[0] != 0)
			m_gpuNotAppliedText = o.gpuNotApplied;
	}

	void GuidanceScheduler::OnCounts(uint64_t pairSeq, const float counts[4], double pairSeconds)
	{
		Stored& p = m_pending[pairSeq % m_pending.size()];
		if (p.seq != pairSeq || !p.result.frame)
			return;
		CameraModel::MeasurementPlan plan = p.result.plan;
		const GuidanceOutcome o = ParseGuidanceCounts(counts, plan.counters.narrowBlocks, EligibleCount(plan));
		if (o.valid)
			plan.counters.escalated = o.escalated;
		m_diag.Add(plan);
		if (m_bench)
		{
			BenchRow row;
			row.pair = pairSeq;
			row.seq = p.rec ? pairSeq : 0;
			row.switchMask = SwitchMask(m_settings.switches);
			row.d = 0;
			row.confidence = p.result.confidence;
			row.reason = p.result.reason;
			row.narrow = plan.counters.narrowBlocks;
			row.full = plan.counters.fullBlocks;
			row.escalated = plan.counters.escalated;
			row.skipped = plan.counters.matcherSkipped ? 1u : 0u;
			row.governorState = m_governor.Tripped() ? 1 : 0;
			row.gpuMs = p.gpuMs;
			m_bench->Write(row);
		}
		GuidedPairResult g;
		g.guided = true;
		g.plannedNarrow = o.narrowPlanned + o.e1Eligible;
		g.escalated = o.escalated;
		g.matcherSkipped = plan.counters.matcherSkipped;
		if (g.matcherSkipped)
		{
			// The skip is confirmed when the pair's own fit also saw the cut / flash. No record yet: cannot judge (not a trip).
			const std::shared_ptr<const CameraModel::CameraMotion> own = m_link.CausalRecord(pairSeq);
			if (own)
			{
				const CameraModel::Reason r = own->reason;
				g.skipUnconfirmed = !(r == CameraModel::Reason::Cut || r == CameraModel::Reason::Flash ||
					r == CameraModel::Reason::PostCutSeeded || r == CameraModel::Reason::FlashBridged || r == CameraModel::Reason::GenerationReset);
			}
		}
		g.pairSeconds = pairSeconds;
		++m_counters.pairsJudged;
		if (!m_governor.Tripped())
			m_governor.OnPair(g);   // while tripped the cool-down is advanced by Guide(); this pair ran without narrow / E1
		RefreshSnapshot();
		p = Stored();
	}

	std::vector<std::string> GuidanceScheduler::StatusLines() const
	{
		std::vector<std::string> lines;
		if (!m_settings.switches.Any())
			return lines;
		if (m_diag.frames > 0)
			lines.push_back(m_diag.StatusLine());
		const std::string g = m_governor.StatusLine();
		if (!g.empty())
			lines.push_back(g);
		char nb[160];
		if (m_counters.narrowIdleEngine > 0)
		{
			std::snprintf(nb, sizeof(nb), "camera guidance: engine field used, narrow idle, %llu pairs (no seed dropped)",
				(unsigned long long)m_counters.narrowIdleEngine);
			lines.push_back(nb);
		}
		if (m_counters.rejectedByMatcher > 0 && !m_rejectedText.empty())
		{
			std::snprintf(nb, sizeof(nb), " (%llu pairs, full search)", (unsigned long long)m_counters.rejectedByMatcher);
			lines.push_back(m_rejectedText + nb);
		}
		if (!m_gpuNotAppliedText.empty())
			lines.push_back("camera guidance: " + m_gpuNotAppliedText);
		{
			uint64_t total = 0;
			for (size_t i = 0; i < size_t(Refusal::Count); ++i)
				total += m_counters.refusals[i];
			if (total > 0)
			{
				const uint64_t guided = m_counters.guided;
				const uint64_t unguided = total > guided ? total - guided : 0;
				const auto& n = m_counters.notOnTime;
				char b[320];
				std::snprintf(b, sizeof(b), "camera guidance (d=0): %llu guided on time (%.0f%% of pairs), %llu unguided - camera(N) not ready: "
					"a %llu, b %llu, c %llu, d %llu, e %llu; R %llu guided then rejected at the seal",
					(unsigned long long)guided, 100.0 * double(guided) / double(total), (unsigned long long)unguided,
					(unsigned long long)n[size_t(OnTimeMiss::NoAheadPair)], (unsigned long long)n[size_t(OnTimeMiss::NotPublished)],
					(unsigned long long)n[size_t(OnTimeMiss::Bridged)], (unsigned long long)n[size_t(OnTimeMiss::Stale)],
					(unsigned long long)n[size_t(OnTimeMiss::NotThePair)], (unsigned long long)m_counters.rejectedAtSeal);
				lines.push_back(b);
			}
		}
		for (size_t i = 1; i < size_t(Refusal::Count); ++i)
			if (i != size_t(Refusal::NotOnTime) && m_counters.refusals[i] > 0)
			{
				char b[160];
				std::snprintf(b, sizeof(b), "camera guidance refused: %s, %llu pairs (full search)", Text(static_cast<Refusal>(i)),
					(unsigned long long)m_counters.refusals[i]);
				lines.push_back(b);
			}
		return lines;
	}

	void CameraGuidanceHook::BeforeEstimate(const VpMotion::EstimateContext& ctx, VpMotion::IGuidanceSink& sink)
	{
		const GuideResult r = m_sched.Guide(ctx.pairSeq, ctx.nextPairSeconds, 0);   // 0: EstimateContext carries no A yet (SameCameraPair)
		sink.SetCameraGuidance(r.frame);   // null = nothing bound: today's matcher exactly
		m_waiting.push_back(std::make_pair(ctx.pairSeq, ctx.nextPairSeconds));
		if (m_waiting.size() > 32)
			m_waiting.erase(m_waiting.begin());
	}

	void CameraGuidanceHook::AfterEstimate(uint64_t pairSeq, const VpMotion::EstimateOutcome& outcome)
	{
		// The engine's plain-data outcome onto the camera's: the engine does not say which pass ran, but it hands the counts texture over
		// only when a narrow or E1 pass wrote it, so a texture means a counting pass ran. narrowIdle is "narrow planned, the engine's
		// field seeded the searches" (shown as "engine field used, narrow idle"); gpuMs is -1 (not measured) today.
		EstimateOutcome o;
		o.guidanceUsed = outcome.guidanceUsed;
		o.narrowPass = outcome.countsTex != nullptr;
		o.narrowDroppedEngine = outcome.narrowIdle;
		o.rejected = outcome.rejection != nullptr ? outcome.rejection : "";
		o.gpuMs = outcome.gpuMs;
		o.counts = outcome.countsTex;
		AfterEstimateEx(pairSeq, o);
	}

	void CameraGuidanceHook::AfterEstimateEx(uint64_t pairSeq, const EstimateOutcome& outcome)
	{
		const bool guidanceUsed = outcome.guidanceUsed;
		const pl_tex countsTex = (outcome.narrowPass || outcome.e1Pass) ? outcome.counts : nullptr;
		m_sched.OnEstimateOutcome(pairSeq, outcome);
		double secs = 0;
		for (const auto& w : m_waiting)
			if (w.first == pairSeq)
				secs = w.second;
		static const float kNone[4] = { -1, -1, -1, -1 };
		if (guidanceUsed)
			++m_used;
		if (guidanceUsed && countsTex && m_counts && m_counts->Request(pairSeq, countsTex))
			return;   // finished in Poll() a frame later
		m_sched.OnCounts(pairSeq, kNone, secs);
	}

	void CameraGuidanceHook::Poll()
	{
		m_sched.PollSeals();
		if (!m_counts)
			return;
		std::vector<IGuidanceCountsSource::Done> done;
		m_counts->Poll(done);
		for (const IGuidanceCountsSource::Done& d : done)
		{
			double secs = 0;
			for (const auto& w : m_waiting)
				if (w.first == d.pairSeq)
					secs = w.second;
			m_sched.OnCounts(d.pairSeq, d.v, secs);
		}
	}
}
