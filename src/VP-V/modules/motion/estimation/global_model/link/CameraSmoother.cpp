// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraSmoother.h"

#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

namespace CameraLink
{
	namespace
	{
		// Keep enough causal records to rebuild the +-kMaxCameraLead smoother window and answer recent queries.
		constexpr size_t kRetainedRecords = static_cast<size_t>(VpSkeleton::kMaxCameraLead) * 4u + 16u;

		// Derived LOO innovation spike base threshold fallback when neither BlockData nor CRD2 carries
		// absThresholdPx: matches Config::absThresholdSigmas (3.0) * PairPrecision (0.25 * blockSize / 16),
		// where nominal blockSize scales with picture raster from 16 px at 1080p (32 px at 4K, 64 px at 8K).
		inline double DerivedLooBaseThresholdPx(const CameraModel::Rect& picture)
		{
			const double w = static_cast<double>(picture.Width());
			const double h = static_cast<double>(picture.Height());
			const double scale = (std::max)(1.0, (std::max)(w / 1920.0, h / 1080.0));
			return 3.0 * (0.25 * scale);
		}

		inline bool BreaksSegment(const CameraModel::CameraMotion& r, const CameraModel::CameraMotion& centre)
		{
			return !r.valid || r.reason == CameraModel::Reason::Cut || r.reason == CameraModel::Reason::Flash
				|| r.reason == CameraModel::Reason::GenerationReset
				|| r.resyncGeneration != centre.resyncGeneration
				|| r.rebaseGeneration != centre.rebaseGeneration
				|| !(r.picture == centre.picture);
		}

		inline bool SameSegmentUsable(const CameraModel::CameraMotion& r, const CameraModel::CameraMotion& centre)
		{
			return !BreaksSegment(r, centre) && CameraModel::ReasonUsable(r.reason) && r.pairSeconds > 0.0;
		}

		struct WeightedMember
		{
			const CameraModel::CameraMotion* rec;
			double w;
		};

		// Interpolates a set of weighted same-segment usable neighbours to `centre.pairSeconds` (zero weight on centre).
		bool InterpolateMembers(const std::vector<WeightedMember>& members, const CameraModel::CameraMotion& centre, CameraModel::Params& out)
		{
			if (members.empty() || !(centre.pairSeconds > 0.0) || centre.picture.Empty())
				return false;
			CameraModel::ModelType highest = CameraModel::ModelType::None;
			for (size_t i = 0; i < members.size(); ++i)
				if (static_cast<int>(members[i].rec->params.type) > static_cast<int>(highest))
					highest = members[i].rec->params.type;
			if (highest == CameraModel::ModelType::None)
				return false;

			const double sec = centre.pairSeconds;
			if (highest != CameraModel::ModelType::Affine && highest != CameraModel::ModelType::Homography)
			{
				double sum[4] = { 0.0, 0.0, 0.0, 0.0 };
				double wsum = 0.0;
				bool allTranslation = true;
				for (size_t i = 0; i < members.size(); ++i)
				{
					const CameraModel::CameraMotion& r = *members[i].rec;
					CameraModel::SimilarityRates rates;
					if (!CameraModel::ToSimilarity(r.params, r.picture, r.pairSeconds, rates))
						rates = r.rates;
					const double w = members[i].w;
					sum[0] += w * rates.logScale;
					sum[1] += w * rates.angle;
					sum[2] += w * rates.tx;
					sum[3] += w * rates.ty;
					wsum += w;
					if (r.params.type != CameraModel::ModelType::Translation)
						allTranslation = false;
				}
				if (!(wsum > 0.0))
					return false;
				CameraModel::SimilarityRates avg;
				avg.logScale = sum[0] / wsum;
				avg.angle = sum[1] / wsum;
				avg.tx = sum[2] / wsum;
				avg.ty = sum[3] / wsum;
				if (allTranslation)
				{
					CameraModel::Params p;
					p.type = CameraModel::ModelType::Translation;
					p.h[2] = avg.tx * sec;
					p.h[5] = avg.ty * sec;
					out = p;
				}
				else
				{
					CameraModel::Params p = CameraModel::FromSimilarity(avg, sec);
					p.type = CameraModel::ModelType::Similarity;
					out = p;
				}
				return true;
			}

			double cdx[4] = { 0.0, 0.0, 0.0, 0.0 }, cdy[4] = { 0.0, 0.0, 0.0, 0.0 };
			double wsum = 0.0;
			for (size_t i = 0; i < members.size(); ++i)
			{
				const CameraModel::CameraMotion& r = *members[i].rec;
				if (!(r.pairSeconds > 0.0))
					continue;
				const double rw = 0.5 * r.picture.Width(), rh = 0.5 * r.picture.Height();
				const double cx[4] = { -rw, rw, -rw, rw };
				const double cy[4] = { -rh, -rh, rh, rh };
				for (int c = 0; c < 4; ++c)
				{
					double dx = 0.0, dy = 0.0;
					CameraModel::Displacement(r.params, cx[c], cy[c], &dx, &dy);
					cdx[c] += members[i].w * dx / r.pairSeconds;
					cdy[c] += members[i].w * dy / r.pairSeconds;
				}
				wsum += members[i].w;
			}
			if (!(wsum > 0.0))
				return false;
			const double hw = 0.5 * centre.picture.Width(), hh = 0.5 * centre.picture.Height();
			const double px[4] = { -hw, hw, -hw, hw };
			const double py[4] = { -hh, -hh, hh, hh };
			CameraModel::Sample s[4];
			for (int c = 0; c < 4; ++c)
			{
				s[c].x = px[c];
				s[c].y = py[c];
				s[c].dx = cdx[c] / wsum * sec;
				s[c].dy = cdy[c] / wsum * sec;
				s[c].w = 1.0;
			}
			CameraModel::Params p;
			if (!CameraModel::SolveMinimal(highest, s, CameraModel::MinimalCount(highest), p))
				return false;
			p.type = highest;
			out = p;
			return true;
		}

		// CE-WATER-C2 (C6): shared leave-one-out symmetric innovation spike detector + symmetric centre-excluded
		// LOO substitution used by both CameraSmoother::Query and MakeP9OwnerFill (HIGH-3 parity).
		struct LooEval
		{
			bool haveLoo = false;
			double innovationPx = 0.0;
			bool spikeFlagged = false;
			CameraModel::Params looHat;
		};

		template <class FindPtrFn>
		LooEval EvaluateLooInnovation(uint64_t seq, const CameraModel::CameraMotion& causal, FindPtrFn&& findPtr)
		{
			LooEval eval;
			if (seq == 0 || !(causal.pairSeconds > 0.0) || causal.picture.Empty())
				return eval;
			const CameraModel::CameraMotion* prev1 = findPtr(seq - 1);
			const CameraModel::CameraMotion* next1 = findPtr(seq + 1);
			if (prev1 && next1 && SameSegmentUsable(*prev1, causal) && SameSegmentUsable(*next1, causal))
			{
				std::vector<WeightedMember> pair1;
				pair1.push_back({ prev1, 1.0 });
				pair1.push_back({ next1, 1.0 });
				if (InterpolateMembers(pair1, causal, eval.looHat))
				{
					eval.haveLoo = true;
					const double diag = std::hypot(static_cast<double>(causal.picture.Width()),
						static_cast<double>(causal.picture.Height()));
					eval.innovationPx = CameraModel::MaxCornerDifference(causal.params, eval.looHat, causal.picture) * diag;
					const double baseThrPx = (causal.blocks && causal.blocks->absThresholdPx > 0.0f)
						? static_cast<double>(causal.blocks->absThresholdPx)
						: (causal.absThresholdPx > 0.0
							? causal.absThresholdPx
							: DerivedLooBaseThresholdPx(causal.picture));
					// Variance of symmetric LOO residual d_t - 0.5*(d_{t-1} + d_{t+1}) for independent pairs
					// is (1 + 1/4 + 1/4) * sigma^2 = 1.5 * sigma^2 -> scale 3-sigma threshold by sqrt(1.5).
					const double looThrPx = baseThrPx * std::sqrt(1.5);
					if (eval.innovationPx > looThrPx)
						eval.spikeFlagged = true;
				}
			}
			return eval;
		}

		template <class FindPtrFn>
		CameraModel::Params SubstituteLooSpike(uint64_t seq, const CameraModel::CameraMotion& causal, int effectiveLead,
			const CameraModel::Params& looHat, FindPtrFn&& findPtr)
		{
			std::vector<WeightedMember> symMembers;
			bool pastBroken = false, futureBroken = false;
			for (int d = 1; d <= effectiveLead; ++d)
			{
				if (pastBroken || futureBroken || seq < static_cast<uint64_t>(d))
					break;
				const CameraModel::CameraMotion* rp = findPtr(seq - static_cast<uint64_t>(d));
				const CameraModel::CameraMotion* rn = findPtr(seq + static_cast<uint64_t>(d));
				if (rp && BreaksSegment(*rp, causal)) pastBroken = true;
				if (rn && BreaksSegment(*rn, causal)) futureBroken = true;
				if (pastBroken || futureBroken)
					break;
				if (rp && rn && SameSegmentUsable(*rp, causal) && SameSegmentUsable(*rn, causal))
				{
					const double w = static_cast<double>(effectiveLead + 1 - d);
					symMembers.push_back({ rp, w });
					symMembers.push_back({ rn, w });
				}
			}
			CameraModel::Params looSub;
			if (!symMembers.empty() && InterpolateMembers(symMembers, causal, looSub))
				return looSub;
			return looHat;
		}
	}

	size_t EncodeSmoothedPod(const CameraModel::CameraMotion& m, uint8_t effectiveLead, uint8_t* out, size_t cap)
	{
		if (!out || cap == 0)
			return 0;
		detail::Writer w(out, cap < kSmoothedPodMaxBytes ? cap : kSmoothedPodMaxBytes);
		w.Put(kSmoothedPodMagic);
		w.Put(m.seq);
		w.Put(m.pairSeconds);
		w.Put(uint8_t(m.valid ? 1 : 0));
		w.Put(uint8_t(m.smoothed ? 1 : 0));
		w.Put(effectiveLead);
		w.Put(uint8_t(m.reason));
		w.Put(m.verdictFlags);
		const CameraModel::Params& active = ActiveParams(m);
		w.Put(uint8_t(active.type));
		for (int i = 0; i < 9; ++i)
			w.Put(active.h[i]);
		w.Put(m.confidence);
		w.Put(int32_t(m.picture.left));
		w.Put(int32_t(m.picture.top));
		w.Put(int32_t(m.picture.right));
		w.Put(int32_t(m.picture.bottom));
		w.Put(m.pairFrom);
		w.Put(m.pairTo);
		w.Put(m.resyncGeneration);
		w.Put(m.rebaseGeneration);
		w.Put(m.rates.logScale);
		w.Put(m.rates.angle);
		w.Put(m.rates.tx);
		w.Put(m.rates.ty);
		w.Put(uint8_t(m.inFade ? 1 : 0));
		detail::PutFadeFields(w, m);
		w.Put(m.absThresholdPx);
		return w.ok ? w.n : 0;
	}

	bool DecodeSmoothedPod(const uint8_t* in, size_t bytes, CameraModel::CameraMotion& m, uint8_t* effectiveLeadOut)
	{
		if (!in || bytes == 0)
			return false;
		detail::Reader r(in, bytes);
		if (r.Get<uint32_t>() != kSmoothedPodMagic)
			return false;
		CameraModel::CameraMotion x;
		x.seq = r.Get<uint64_t>();
		x.pairSeconds = r.Get<double>();
		x.valid = r.Get<uint8_t>() != 0;
		x.smoothed = r.Get<uint8_t>() != 0;
		const uint8_t lead = r.Get<uint8_t>();
		const uint8_t reason = r.Get<uint8_t>();
		const uint8_t verdictFlags = r.Get<uint8_t>();
		const uint8_t type = r.Get<uint8_t>();
		if (reason > kRecordPodLastReason || type > kRecordPodLastModelType)
			return false;
		x.reason = static_cast<CameraModel::Reason>(reason);
		x.verdictFlags = verdictFlags;
		CameraModel::Params active;
		active.type = static_cast<CameraModel::ModelType>(type);
		for (int i = 0; i < 9; ++i)
			active.h[i] = r.Get<double>();
		x.confidence = r.Get<double>();
		x.picture.left = r.Get<int32_t>();
		x.picture.top = r.Get<int32_t>();
		x.picture.right = r.Get<int32_t>();
		x.picture.bottom = r.Get<int32_t>();
		x.pairFrom = r.Get<uint64_t>();
		x.pairTo = r.Get<uint64_t>();
		x.resyncGeneration = r.Get<uint64_t>();
		x.rebaseGeneration = r.Get<uint64_t>();
		x.rates.logScale = r.Get<double>();
		x.rates.angle = r.Get<double>();
		x.rates.tx = r.Get<double>();
		x.rates.ty = r.Get<double>();
		x.inFade = r.Get<uint8_t>() != 0;
		detail::GetFadeFields(r, x);
		x.absThresholdPx = r.Get<double>();
		if (!r.ok)
			return false;
		x.params = active;
		if (x.smoothed)
			x.smoothedParams = active;
		m = x;
		if (effectiveLeadOut)
			*effectiveLeadOut = lead;
		return true;
	}

	const char* const kP9OwnerUnit = "motion/estimation/global_model";

	P9OwnerCounters::P9OwnerCounters() : fills(0), smoothed(0), causalFallback(0), declines(0) {}

	VpSkeleton::OwnerFill MakeP9OwnerFill() { return MakeP9OwnerFill(int(VpSkeleton::kMaxCameraLead), nullptr); }
	VpSkeleton::OwnerFill MakeP9OwnerFill(int laneLead) { return MakeP9OwnerFill(laneLead, nullptr); }
	bool RegisterP9OwnerFill() { return RegisterP9OwnerFill(nullptr, int(VpSkeleton::kMaxCameraLead), nullptr); }
	bool RegisterP9OwnerFill(Vpv::Skeleton::IExternalDemand* demand) { return RegisterP9OwnerFill(demand, int(VpSkeleton::kMaxCameraLead), nullptr); }

	CameraSmoother::CameraSmoother(int laneLead)
		: m_effectiveLead(EffectiveCameraLead(laneLead))
		, m_smoother(m_effectiveLead)
	{
	}

	void CameraSmoother::SetLaneLead(int laneLead)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const int nextLead = EffectiveCameraLead(laneLead);
		if (nextLead == m_effectiveLead)
			return;
		m_effectiveLead = nextLead;
		RebuildSmootherLocked();
	}

	int CameraSmoother::EffectiveLead() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_effectiveLead;
	}

	void CameraSmoother::Reset()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_smoother.Reset();
		m_records.clear();
		m_counters.resets.fetch_add(1, std::memory_order_relaxed);
	}

	void CameraSmoother::RebuildSmootherLocked()
	{
		m_smoother = CameraModel::FixedLagSmoother(m_effectiveLead);
		for (const std::shared_ptr<const CameraModel::CameraMotion>& r : m_records)
			if (r)
				m_smoother.Push(*r);
	}

	void CameraSmoother::Push(const std::shared_ptr<const CameraModel::CameraMotion>& record)
	{
		if (!record)
			return;
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_records.empty() && record->seq <= m_records.back()->seq)
		{
			// Non-monotonic sequence (seek / restart / rebuild): reset cleanly before pushing.
			m_smoother.Reset();
			m_records.clear();
			m_counters.resets.fetch_add(1, std::memory_order_relaxed);
		}
		m_records.push_back(record);
		while (m_records.size() > kRetainedRecords)
			m_records.pop_front();
		m_smoother.Push(*record);
		m_counters.pushed.fetch_add(1, std::memory_order_relaxed);
	}

	bool CameraSmoother::PushFromP7Slot(const VpSkeleton::LaneLease& p7Lease, uint64_t seq)
	{
		if (!p7Lease.table || !p7Lease.bytes)
			return false;
		VpSkeleton::SlotSet* set = p7Lease.table->Find(VpSkeleton::kP7CameraFit);
		if (!set)
			return false;
		const VpSkeleton::SlotState st = set->StateOf(seq);
		if (st != VpSkeleton::SlotState::Published && st != VpSkeleton::SlotState::Sealed)
			return false;
		const uint16_t slot = set->SlotOf(seq);
		const uint8_t* raw = p7Lease.bytes->Ring(static_cast<size_t>(slot));
		if (!raw)
			return false;
		auto rec = std::make_shared<CameraModel::CameraMotion>();
		if (!DecodeRecordPod(raw, p7Lease.bytes->Bytes(), *rec))
			return false;
		Push(rec);
		return true;
	}

	bool CameraSmoother::Query(uint64_t seq, SmoothQueryResult& out) const
	{
		out = SmoothQueryResult();
		std::lock_guard<std::mutex> lock(m_mutex);
		out.effectiveLead = m_effectiveLead;
		auto findRec = [&](uint64_t s) -> std::shared_ptr<const CameraModel::CameraMotion>
		{
			for (const std::shared_ptr<const CameraModel::CameraMotion>& r : m_records)
				if (r && r->seq == s)
					return r;
			return nullptr;
		};
		std::shared_ptr<const CameraModel::CameraMotion> causal = findRec(seq);
		if (!causal)
		{
			m_counters.missing.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (m_effectiveLead <= 0)
		{
			// Queue / lane depth 0: causal P7 is served with visible reduced-mode status.
			out.record = causal;
			out.smoothed = false;
			out.causalShortQueue = true;
			m_counters.causalShortQueue.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		// Never smooth or mark smoothed = true on an invalid or unusable centre record (Ambiguous, LowConfidence, Cut, Flash, ...).
		if (!causal->valid || !CameraModel::ReasonUsable(causal->reason))
		{
			out.record = causal;
			out.smoothed = false;
			return true;
		}

		// CE-WATER-C2 (C6): leave-one-out symmetric innovation H_hat_t = 0.5 * (H_{t-1} + H_{t+1}).
		auto findPtr = [&](uint64_t s) -> const CameraModel::CameraMotion* {
			for (size_t i = m_records.size(); i > 0; --i)
				if (m_records[i - 1] && m_records[i - 1]->seq == s)
					return m_records[i - 1].get();
			return nullptr;
		};
		const LooEval loo = EvaluateLooInnovation(seq, *causal, findPtr);
		out.haveLoo = loo.haveLoo;
		out.looInnovationPx = loo.innovationPx;
		out.looSpikeFlagged = loo.spikeFlagged;

		CameraModel::Params sp;
		if (m_smoother.Smoothed(seq, sp))
		{
			if (loo.spikeFlagged && loo.haveLoo)
			{
				sp = SubstituteLooSpike(seq, *causal, m_effectiveLead, loo.looHat, findPtr);
				m_counters.looSpikes.fetch_add(1, std::memory_order_relaxed);
			}
			auto rec = std::make_shared<CameraModel::CameraMotion>(*causal);
			rec->smoothed = true;
			rec->smoothedParams = sp;
			out.record = std::move(rec);
			out.smoothed = true;
			m_counters.smoothed.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		// Future neighbours N+1..N+k have not arrived yet: serve causal P7 visibly, never block.
		out.record = causal;
		out.smoothed = false;
		out.causalWindowShort = true;
		m_counters.causalWindowShort.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool CameraSmoother::PublishSlot(const VpSkeleton::LaneLease& p9Lease, uint64_t seq, const VpIndicator::Stamp& fallbackStamp) const
	{
		auto refuse = [&]() {
			m_counters.slotRefused.fetch_add(1, std::memory_order_relaxed);
			return false;
		};
		if (!p9Lease.table)
			return refuse();
		VpSkeleton::SlotSet* set = p9Lease.table->Find(VpSkeleton::kP9CameraSmoothed);
		if (!set)
			return refuse();
		SmoothQueryResult q;
		if (!Query(seq, q) || !q.record)
			return refuse();
		VpSkeleton::LaneTicket ticket;
		if (set->Claim(seq, VpAncestry::Provenance::AheadLane, ~uint64_t(0), &ticket) != VpSkeleton::ClaimResult::Claimed)
			return refuse();
		if (!set->Writable(ticket))
		{
			set->Abandon(ticket);
			return refuse();
		}
		VpAncestry::Header& hdr = set->LaneHeader(ticket);
		VpSkeleton::LaneInputs pins;
		if (!p9Lease.table->LaneAncestors(VpSkeleton::kP9CameraSmoothed, seq, hdr, pins))
		{
			pins.Unpin();
			set->Abandon(ticket);
			return refuse();
		}
		hdr.honoured = p9Lease.indicator ? p9Lease.indicator->StampOf(seq) : fallbackStamp;
		if (p9Lease.bytes)
		{
			uint8_t* dst = p9Lease.bytes->Ring(static_cast<size_t>(ticket.slot));
			if (dst && EncodeSmoothedPod(*q.record, static_cast<uint8_t>(q.effectiveLead), dst, p9Lease.bytes->Bytes()) == 0)
			{
				pins.Unpin();
				set->Abandon(ticket);
				return refuse();
			}
		}
		const VpSkeleton::PublishResult pr = set->Publish(ticket);
		pins.Unpin();
		if (pr != VpSkeleton::PublishResult::Published)
			return refuse();
		m_counters.slotPublished.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	VpSkeleton::OwnerFill MakeP9OwnerFill(int laneLead, P9OwnerCounters* counters)
	{
		const int effectiveLead = EffectiveCameraLead(laneLead);
		VpSkeleton::OwnerFill f;
		f.variantAxes = 0;
		f.renderThreadOnly = false;
		f.keepFrames = 1;
		f.fill = [effectiveLead, counters](const VpSkeleton::OwnerFillRequest& req,
			VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) -> bool
		{
			auto decline = [&](const std::string& why) -> bool {
				out.declineReason = why;
				if (counters != nullptr)
					counters->declines.fetch_add(1, std::memory_order_relaxed);
				return false;
			};
			const VpSkeleton::AcquireResult centreRes = memo.Acquire(
				VpSkeleton::kP7CameraFit, req.seq, VpSkeleton::AcquireForm::Cpu);
			if (!centreRes.Ok() || centreRes.cpu == nullptr || centreRes.cpuBytes == 0)
				return decline(std::string("p9_camera_smoothed: P7 unavailable (")
					+ VpSkeleton::Token(centreRes.cause) + ")");
			CameraModel::CameraMotion centre;
			if (!DecodeRecordPod(centreRes.cpu, centreRes.cpuBytes, centre))
				return decline("p9_camera_smoothed: P7 record decode failed");

			CameraModel::CameraMotion result = centre;
			result.smoothed = false;
			uint8_t usedLead = 0;
			int activeLead = effectiveLead;
			// HIGH-3: respect the memo's queue window on short queues / Games so out-of-range [seq-4..seq+4]
			// neighbours are never polled (avoiding SkeletonMemo::Fail() pollution of lastCause_[kP7CameraFit]).
			const VpSkeleton::QueueWindow w = memo.Window();
			const bool hasWindow = (w.wired != 0 || w.current != 0 || w.ahead != 0 || w.back != 0);
			if (hasWindow && activeLead > 0)
			{
				const uint16_t maxAhead = w.ahead < w.aheadCompute ? w.ahead : w.aheadCompute;
				if (int(maxAhead) < activeLead)
					activeLead = int(maxAhead);
			}
			if (activeLead > 0 && centre.valid && CameraModel::ReasonUsable(centre.reason))
			{
				CameraModel::FixedLagSmoother smoother(activeLead);
				const uint64_t lag = static_cast<uint64_t>(activeLead);
				const uint64_t lo = req.seq > lag ? (req.seq - lag) : 0u;
				const uint64_t hi = req.seq <= ((std::numeric_limits<uint64_t>::max)() - lag)
					? (req.seq + lag) : (std::numeric_limits<uint64_t>::max)();
				static constexpr size_t kMaxWindow = static_cast<size_t>(VpSkeleton::kMaxCameraLead) * 2u + 1u;
				std::array<CameraModel::CameraMotion, kMaxWindow> windowRecs{};
				std::array<bool, kMaxWindow> windowValid{};
				for (uint64_t s = lo; s <= hi; ++s)
				{
					const size_t idx = static_cast<size_t>(s - lo);
					if (s == req.seq)
					{
						if (idx < kMaxWindow) { windowRecs[idx] = centre; windowValid[idx] = true; }
						smoother.Push(centre);
						if (s == hi) break;
						continue;
					}
					// Non-mutating guard before Acquire: never trigger Fail() for released or unadmitted ahead frames.
					const bool ready = memo.HasReadyCpu(VpSkeleton::kP7CameraFit, s);
					if (!ready)
					{
						if (memo.Released(s))
						{
							if (s == hi) break;
							continue;
						}
						if (memo.AheadOfSeal(s))
						{
							VpSkeleton::AcquireCause why = VpSkeleton::AcquireCause::None;
							if (!memo.CheckAheadAdmitted(VpSkeleton::kP7CameraFit, s, why))
							{
								if (s == hi) break;
								continue;
							}
						}
					}
					const VpSkeleton::AcquireResult nr = memo.Acquire(
						VpSkeleton::kP7CameraFit, s, VpSkeleton::AcquireForm::Cpu);
					if (nr.Ok() && nr.cpu != nullptr && nr.cpuBytes > 0)
					{
						CameraModel::CameraMotion nrec;
						if (DecodeRecordPod(nr.cpu, nr.cpuBytes, nrec))
						{
							if (idx < kMaxWindow) { windowRecs[idx] = nrec; windowValid[idx] = true; }
							smoother.Push(nrec);
						}
					}
					if (s == hi) break;
				}
				CameraModel::Params smoothedParams;
				if (smoother.Smoothed(req.seq, smoothedParams))
				{
					// HIGH-3: CE-WATER-C2 LOO spike filter parity with CameraSmoother::Query.
					struct WindowRecFinder
					{
						uint64_t lo;
						uint64_t hi;
						const CameraModel::CameraMotion* recs;
						const bool* valid;
						size_t count;
						const CameraModel::CameraMotion* operator()(uint64_t s) const
						{
							if (s < lo || s > hi) return nullptr;
							const size_t idx = static_cast<size_t>(s - lo);
							return (idx < count && valid[idx]) ? &recs[idx] : nullptr;
						}
					};
					const WindowRecFinder findWindowRec{ lo, hi, windowRecs.data(), windowValid.data(), windowValid.size() };
					const LooEval loo = EvaluateLooInnovation(req.seq, centre, findWindowRec);
					if (loo.spikeFlagged && loo.haveLoo)
						smoothedParams = SubstituteLooSpike(req.seq, centre, activeLead, loo.looHat, findWindowRec);
					result.smoothed = true;
					result.smoothedParams = smoothedParams;
					usedLead = static_cast<uint8_t>(activeLead);
				}
			}

			uint8_t buf[kSmoothedPodMaxBytes]{};
			const size_t n = EncodeSmoothedPod(result, usedLead, buf, sizeof(buf));
			if (n == 0)
				return decline("p9_camera_smoothed: smoothed POD encode failed");
			out.bytes.assign(buf, buf + n);
			if (counters != nullptr)
			{
				counters->fills.fetch_add(1, std::memory_order_relaxed);
				if (result.smoothed)
					counters->smoothed.fetch_add(1, std::memory_order_relaxed);
				else
					counters->causalFallback.fetch_add(1, std::memory_order_relaxed);
			}
			return true;
		};
		return f;
	}

	bool RegisterP9OwnerFill(Vpv::Skeleton::IExternalDemand* demand, int laneLead, P9OwnerCounters* counters)
	{
		if (demand == nullptr)
			demand = static_cast<Vpv::Skeleton::IExternalDemand*>(
				Vpv::Hub::ExtensionHub::Instance().Query(Vpv::Skeleton::kExternalDemandQueryId));
		if (demand == nullptr)
			return false;
		const VpSkeleton::SkeletonMemo::Registration r = demand->RegisterOwnerFill(
			VpSkeleton::kP9CameraSmoothed, VpSkeleton::AcquireForm::Cpu, kP9OwnerUnit,
			MakeP9OwnerFill(laneLead, counters));
		return r == VpSkeleton::SkeletonMemo::Registration::Registered
			|| r == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered;
	}

	namespace
	{
		std::string P9RegistrationText(VpSkeleton::SkeletonMemo::Registration r)
		{
			using R = VpSkeleton::SkeletonMemo::Registration;
			switch (r)
			{
			case R::Registered: return "registered";
			case R::Replaced: return "replaced";
			case R::NotOwner: return "refused: not the catalog's owning unit";
			case R::NotInCatalog: return "refused: not in the catalog";
			case R::AlreadyRegistered: return "refused: another fill is already registered";
			case R::BadDefaults: return "refused: bad defaults";
			}
			return "refused";
		}

		class P9OwnerModule : public Vpv::Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "motion.estimation.p9_camera_smoothed"; }
			void OnLifecycle(RendererExtension::LifecycleFacts& f) override
			{
				if (!f.host)
					return;
				if (f.kind == RendererExtension::Lifecycle::Shutdown || f.kind == RendererExtension::Lifecycle::GpuReleased)
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_hostProblems.erase(f.host);
					const auto it = m_hostMemo.find(f.host);
					if (it != m_hostMemo.end())
					{
						m_memoDeclines.erase(it->second);
						m_hostMemo.erase(it);
					}
				}
			}
			bool OnFrame(RendererExtension::FrameContext& c) override
			{
				if (c.phase != RendererExtension::FramePhase::Entrance
					|| Vpv::Hub::ExtensionHub::Instance().OriginalPathway(c.host))
					return false;
				auto* demand = static_cast<Vpv::Skeleton::IExternalDemand*>(
					Vpv::Hub::ExtensionHub::Instance().Query(Vpv::Skeleton::kExternalDemandQueryId));
				if (demand == nullptr)
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_hostProblems[c.host] = "no skeleton to register with (retried at next frame)";
					return false;
				}
				{
					const Vpv::Skeleton::FrameMemo fm = demand->MemoFor(c.host, static_cast<uint64_t>(c.seq > 0 ? c.seq : 0));
					if (fm.Valid())
					{
						std::lock_guard<std::mutex> lock(m_mutex);
						m_hostMemo[c.host] = fm.memo.get();
					}
				}
				if (m_registered.load(std::memory_order_acquire))
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_hostProblems.erase(c.host);
					return false;
				}
				VpSkeleton::OwnerFill fill = MakeP9OwnerFill(int(VpSkeleton::kMaxCameraLead), nullptr);
				auto baseFill = std::move(fill.fill);
				fill.fill = [this, baseFill](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo,
					VpSkeleton::OwnerFillOutput& out) -> bool {
					const bool ok = baseFill(req, memo, out);
					if (!ok)
					{
						std::lock_guard<std::mutex> lock(m_mutex);
						++m_memoDeclines[&memo];
					}
					return ok;
				};
				const VpSkeleton::SkeletonMemo::Registration r = demand->RegisterOwnerFill(
					VpSkeleton::kP9CameraSmoothed, VpSkeleton::AcquireForm::Cpu, kP9OwnerUnit, std::move(fill));
				if (r == VpSkeleton::SkeletonMemo::Registration::Registered
					|| r == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered)
				{
					m_registered.store(true, std::memory_order_release);
					std::lock_guard<std::mutex> lock(m_mutex);
					m_hostProblems.erase(c.host);
				}
				else
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_hostProblems[c.host] = "P9 owner fill " + P9RegistrationText(r);
				}
				return false;
			}
			void OnStatus(const RendererExtension::Context& c, RendererExtension::StatusKind kind,
				std::vector<std::string>& lines, const char*, const char*) override
			{
				// HIGH-3: guard OnStatus with OriginalPathway(c.host) and report per-host counters only.
				if (kind != RendererExtension::StatusKind::Fallbacks
					|| Vpv::Hub::ExtensionHub::Instance().OriginalPathway(c.host))
					return;
				std::string problem;
				uint64_t dec = 0;
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					const auto pit = m_hostProblems.find(c.host);
					if (pit != m_hostProblems.end())
						problem = pit->second;
					const auto mit = m_hostMemo.find(c.host);
					if (mit != m_hostMemo.end())
					{
						const auto dit = m_memoDeclines.find(mit->second);
						if (dit != m_memoDeclines.end())
							dec = dit->second;
					}
				}
				if (!problem.empty())
					lines.push_back("p9_camera_smoothed_unregistered: " + problem);
				if (dec > 0)
					lines.push_back("p9_camera_smoothed_declined: " + std::to_string(dec) + " owner fill declines");
			}
		private:
			mutable std::mutex m_mutex;
			std::map<const void*, std::string> m_hostProblems;
			std::map<const void*, const void*> m_hostMemo;
			std::map<const void*, uint64_t> m_memoDeclines;
			std::atomic<bool> m_registered{ false };
		};
	}

	void RegisterP9OwnerModule()
	{
		static bool registered = false;
		if (registered)
			return;
		registered = true;
		Vpv::Hub::ExtensionHub::Instance().AddModule(
			std::unique_ptr<Vpv::Hub::ISkeletonModule>(new P9OwnerModule()));
	}
}
