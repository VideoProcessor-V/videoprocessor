// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraSmoother.h"

#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>

#include <algorithm>
#include <cmath>

namespace CameraLink
{
	namespace
	{
		// Keep enough causal records to rebuild the +-kMaxCameraLead smoother window and answer recent queries.
		constexpr size_t kRetainedRecords = static_cast<size_t>(VpSkeleton::kMaxCameraLead) * 4u + 16u;

		// Derived LOO innovation spike threshold fallback when BlockData is not attached (e.g. POD-decoded P7):
		// matches Config::absThresholdSigmas (3.0) * nominal 16-px precision (1/4 px) = 0.75 source px.
		constexpr double kLooDefaultAbsThresholdPx = 3.0 / 4.0;

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

		// CE-WATER-C2 (C6): leave-one-out symmetric innovation H_hat_t = 0.5 * (H_{t-1} + H_{t+1}) when both
		// immediate neighbours t-1 and t+1 are usable same-segment records (avoids centre-frame L+1 self-attenuation).
		CameraModel::Params looHat;
		bool haveLooHat = false;
		if (seq > 0 && causal->pairSeconds > 0.0 && !causal->picture.Empty())
		{
			const std::shared_ptr<const CameraModel::CameraMotion> prev1 = findRec(seq - 1);
			const std::shared_ptr<const CameraModel::CameraMotion> next1 = findRec(seq + 1);
			if (prev1 && next1 && SameSegmentUsable(*prev1, *causal) && SameSegmentUsable(*next1, *causal))
			{
				std::vector<WeightedMember> pair1;
				pair1.push_back({ prev1.get(), 1.0 });
				pair1.push_back({ next1.get(), 1.0 });
				if (InterpolateMembers(pair1, *causal, looHat))
				{
					haveLooHat = true;
					out.haveLoo = true;
					const double diag = std::hypot(static_cast<double>(causal->picture.Width()),
						static_cast<double>(causal->picture.Height()));
					out.looInnovationPx = CameraModel::MaxCornerDifference(causal->params, looHat, causal->picture) * diag;
					const double looThrPx = (causal->blocks && causal->blocks->absThresholdPx > 0.0f)
						? static_cast<double>(causal->blocks->absThresholdPx)
						: kLooDefaultAbsThresholdPx;
					if (out.looInnovationPx > looThrPx)
						out.looSpikeFlagged = true;
				}
			}
		}

		CameraModel::Params sp;
		if (m_smoother.Smoothed(seq, sp))
		{
			if (out.looSpikeFlagged && haveLooHat)
			{
				// Substitute the flagged single-frame spike with the symmetric centre-excluded LOO interpolation
				// over all usable same-segment symmetric pairs (t-d, t+d) for d = 1..m_effectiveLead (reduces
				// identically to looHat = 0.5 * (H_{t-1} + H_{t+1}) when lead == 1 or when d >= 2 hits a boundary).
				std::vector<WeightedMember> symMembers;
				bool pastBroken = false, futureBroken = false;
				for (int d = 1; d <= m_effectiveLead; ++d)
				{
					if (pastBroken || futureBroken || seq < static_cast<uint64_t>(d))
						break;
					const std::shared_ptr<const CameraModel::CameraMotion> rp = findRec(seq - static_cast<uint64_t>(d));
					const std::shared_ptr<const CameraModel::CameraMotion> rn = findRec(seq + static_cast<uint64_t>(d));
					if (rp && BreaksSegment(*rp, *causal)) pastBroken = true;
					if (rn && BreaksSegment(*rn, *causal)) futureBroken = true;
					if (pastBroken || futureBroken)
						break;
					if (rp && rn && SameSegmentUsable(*rp, *causal) && SameSegmentUsable(*rn, *causal))
					{
						const double w = static_cast<double>(m_effectiveLead + 1 - d);
						symMembers.push_back({ rp.get(), w });
						symMembers.push_back({ rn.get(), w });
					}
				}
				CameraModel::Params looSub;
				if (!symMembers.empty() && InterpolateMembers(symMembers, *causal, looSub))
					sp = looSub;
				else
					sp = looHat;
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
}
