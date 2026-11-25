// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K9-1, B3 / L-B4-P9-FILL): P9 producer (kP9CameraSmoothed). Smooths P7 camera records over
// N +- k (k = min(kMaxCameraLead, laneLead)) via FixedLagSmoother; causal P7 fallback when queue/window is short.
// Provides 256-byte POD codec (EncodeSmoothedPod/DecodeSmoothedPod), SlotTable publisher, and kP9CameraSmoothed CPU owner fill.
// Pure C++14, deterministic, thread-safe.
#pragma once

#include "CameraRecordPool.h"

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/LaneLease.h>
#include <VP-V/core/ProductSlots.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelTemporal.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace VpSkeleton
{
	struct OwnerFill;
}
namespace Vpv
{
	namespace Skeleton
	{
		class IExternalDemand;
	}
}

namespace CameraLink
{
	extern const char* const kP9OwnerUnit;
	struct P9OwnerCounters
	{
		std::atomic<uint64_t> fills;
		std::atomic<uint64_t> smoothed;
		std::atomic<uint64_t> causalFallback;
		std::atomic<uint64_t> declines;
		P9OwnerCounters();
	};
	VpSkeleton::OwnerFill MakeP9OwnerFill();
	VpSkeleton::OwnerFill MakeP9OwnerFill(int laneLead);
	VpSkeleton::OwnerFill MakeP9OwnerFill(int laneLead, P9OwnerCounters* counters);
	bool RegisterP9OwnerFill();
	bool RegisterP9OwnerFill(Vpv::Skeleton::IExternalDemand* demand);
	bool RegisterP9OwnerFill(Vpv::Skeleton::IExternalDemand* demand, int laneLead, P9OwnerCounters* counters);
	void RegisterP9OwnerModule();

	// Visible reduced-mode status line and FallbackCatalog id when P9 runs causally because the queue is short.
	constexpr const char* kCausalShortQueueText = "camera: causal (queue short)";
	constexpr const char* kIdSmoothedCausal = "camera.smoothed.causal";

	// Declared slot size of kP9CameraSmoothed (core/Products.h: p9.size.fixedBytes = 256).
	constexpr size_t kSmoothedPodMaxBytes = 256;
	constexpr uint32_t kSmoothedPodMagic = 0x43534D31u;   // "CSM1"

	// Derives the effective smoothing half-window k from the measured lane / queue lead (clamped to [0, kMaxCameraLead]).
	inline int EffectiveCameraLead(int laneLead)
	{
		if (laneLead <= 0)
			return 0;
		const int cap = static_cast<int>(VpSkeleton::kMaxCameraLead);
		return laneLead < cap ? laneLead : cap;
	}

	// Returns the active motion parameters of a P9/P7 record: smoothedParams when smoothed, else causal params.
	inline const CameraModel::Params& ActiveParams(const CameraModel::CameraMotion& rec)
	{
		return rec.smoothed ? rec.smoothedParams : rec.params;
	}

	// Encodes the P9 smoothed camera slot payload (<= kSmoothedPodMaxBytes = 256 B). Returns byte count, or 0 on overflow.
	size_t EncodeSmoothedPod(const CameraModel::CameraMotion& m, uint8_t effectiveLead, uint8_t* out, size_t cap);

	// Decodes the P9 smoothed camera slot payload into `m` (updating seq, valid, smoothed, params, smoothedParams,
	// confidence, reason, verdictFlags, picture, pairSeconds) and optional `effectiveLeadOut`. False = bad magic / short / out-of-range enum.
	bool DecodeSmoothedPod(const uint8_t* in, size_t bytes, CameraModel::CameraMotion& m, uint8_t* effectiveLeadOut = nullptr);

	struct SmootherCounters
	{
		std::atomic<uint64_t> pushed{ 0 };
		std::atomic<uint64_t> smoothed{ 0 };           // queries satisfied with a look-ahead smoothed P9 model
		std::atomic<uint64_t> causalShortQueue{ 0 };   // queries that used causal P7 because effectiveLead == 0
		std::atomic<uint64_t> causalWindowShort{ 0 };  // queries that used causal P7 because N+1..N+k had not arrived yet
		std::atomic<uint64_t> missing{ 0 };            // queries with no causal P7 record for seq
		std::atomic<uint64_t> slotPublished{ 0 };      // P9 slots published into a SlotTable
		std::atomic<uint64_t> slotRefused{ 0 };        // P9 slot publish attempts refused (no product / late / stale / ancestors)
		std::atomic<uint64_t> resets{ 0 };             // smoother resets (explicit or seq discontinuity)
		std::atomic<uint64_t> looSpikes{ 0 };          // CE-WATER-C2 (C6): single-frame LOO innovation spikes flagged & substituted
	};

	struct SmoothQueryResult
	{
		std::shared_ptr<const CameraModel::CameraMotion> record;
		bool smoothed = false;           // true: look-ahead P9 smoothed over N +- k
		bool causalShortQueue = false;   // true: queue / lane lead is 0 ("camera: causal (queue short)")
		bool causalWindowShort = false;  // true: lead > 0, but future neighbours not yet available
		int effectiveLead = 0;           // k = min(kMaxCameraLead, laneLead)
		// CE-WATER-C2 (C6): leave-one-out symmetric innovation when both t-1 and t+1 are usable same-segment neighbours.
		bool haveLoo = false;            // true when symmetric usable neighbours t-1 and t+1 were available
		double looInnovationPx = 0.0;    // max corner/centre displacement (source px) between H_t and LOO H_hat_t = 0.5*(H_{t-1} + H_{t+1})
		bool looSpikeFlagged = false;    // true when looInnovationPx > absThresholdSigmas * precisionPx (spike substituted cleanly)
	};

	// Visible status line for the P9 smoother (Ctrl+I / Config status / Ctrl+Alt+D).
	inline std::string SmootherStatusLine(const SmootherCounters& c, int effectiveLead)
	{
		if (effectiveLead <= 0)
			return std::string(kCausalShortQueueText);
		char b[320];
		std::snprintf(b, sizeof(b), "camera smoothed (P9, lead +- %d): %llu smoothed, %llu causal (window short), %llu causal (queue short), %llu LOO spikes, %llu published, %llu refused",
			effectiveLead,
			static_cast<unsigned long long>(c.smoothed.load()),
			static_cast<unsigned long long>(c.causalWindowShort.load()),
			static_cast<unsigned long long>(c.causalShortQueue.load()),
			static_cast<unsigned long long>(c.looSpikes.load()),
			static_cast<unsigned long long>(c.slotPublished.load()),
			static_cast<unsigned long long>(c.slotRefused.load()));
		return b;
	}

	// Visible FallbackCatalog decision: engaged whenever P9 is active in causal-short-queue mode (effectiveLead == 0).
	inline FallbackCatalog::Decision SmootherDecision(int effectiveLead, bool p9Wanted)
	{
		if (p9Wanted && effectiveLead <= 0)
			return FallbackCatalog::Engaged(kIdSmoothedCausal, kCausalShortQueueText);
		return FallbackCatalog::Released(kIdSmoothedCausal);
	}

	class CameraSmoother
	{
	public:
		// `laneLead`: measured queue / lane look-ahead depth; effective lead = min(kMaxCameraLead, laneLead).
		explicit CameraSmoother(int laneLead = 0);

		// Updates the effective lead from the measured queue / lane lead (rebuilds the smoothing window from retained history).
		void SetLaneLead(int laneLead);
		int EffectiveLead() const;
		bool IsCausalShortQueue() const { return EffectiveLead() == 0; }

		void Reset();

		// Feeds a P7 record (monotonic increasing seq; backwards / duplicate seq resets the window cleanly).
		void Push(const std::shared_ptr<const CameraModel::CameraMotion>& record);

		// Convenience: decodes P7's slot bytes for `seq` from a P7 lease and pushes the decoded record.
		bool PushFromP7Slot(const VpSkeleton::LaneLease& p7Lease, uint64_t seq);

		// Queries the smoothed P9 record for `seq`. Returns true when a record (smoothed or causal P7 fallback) is available.
		bool Query(uint64_t seq, SmoothQueryResult& out) const;

		// Publishes P9(seq) into a SlotTable lease containing kP9CameraSmoothed (Claim -> LaneAncestors -> EncodeSmoothedPod -> Publish).
		// Returns true when published; false on refusal (counted visibly in slotRefused).
		bool PublishSlot(const VpSkeleton::LaneLease& p9Lease, uint64_t seq, const VpIndicator::Stamp& fallbackStamp) const;

		const SmootherCounters& Counters() const { return m_counters; }
		std::string StatusLine() const { return SmootherStatusLine(m_counters, EffectiveLead()); }

	private:
		void RebuildSmootherLocked();

		mutable std::mutex m_mutex;
		int m_effectiveLead = 0;
		CameraModel::FixedLagSmoother m_smoother;
		std::deque<std::shared_ptr<const CameraModel::CameraMotion>> m_records;
		mutable SmootherCounters m_counters;
	};
}
