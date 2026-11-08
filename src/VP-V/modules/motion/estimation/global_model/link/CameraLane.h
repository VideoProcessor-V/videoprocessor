// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// *** LAG BRIDGE UNTIL SK-W2 *** (camera-live step 2). The camera's products are held through the skeleton's APIs (SK-W): the fit and
// the label fill hold their table through a LaneLease and rebind on a generation change
// (CameraLinkCore::AttachLane, CameraLabelFill(LaneLeaseSource, ...)), and the costs go to the pass-timer inbox. What this class still
// does is ONLY what SK-W does not provide: the LAGGED SEAL. The skeleton lane seals every product of frame N at the render start of N
// (SlotTable::Resolve; SlotSet::Seal has no deferral), but P7(N) is fitted from a field the engine made while rendering N and read
// back a frame or two later, so in the skeleton's table P7 would be sealed absent on every frame (which is also why the camera declares no
// skeleton demand yet: link/README.md "No skeleton demand yet"). Until SK-W2 seals P7(N) at N + lag in
// the skeleton lane, the fit and the labels are published into THIS small table, whose seal runs kSealLag frames behind the render:
//   * a graph built from the declarations SK-W2 gives the skeleton (LiveCameraDemand), so it holds P7 (+ P8) and their inputs;
//   * input markers: empty-content P3 / P4 / P2m / P1 slots per frame with the honoured stamp, because a lagged seal can only check
//     P7's ancestry inside its own table (the skeleton's P3 / P4 slots are sealed and released on the skeleton's own schedule);
//   * the lagged seal itself (SealOne at N + kSealLag, with the Indicator's stamp OF THAT FRAME) and the field marks that tell a
//     cadence repeat from a late fit; the P8 live fill at that seal (FillLive), NextFloor, LabelRing / LabelLive and ReleaseAfter (N2);
//   * Lease(): an OWNED lease (shared hold of the state + a generation bumped at every Build / Clear), so the holders use exactly the
//     lease path they will use on the skeleton lane (AnalysisLane::Lease) and a rebuild is a StaleLane refusal, never a freed table.
// SK-W2 REMOVES this file: the lease source becomes [lane]{ return lane->Lease(kP7CameraFit); } (and kP8CameraLabels), the extension's
// per-frame OnFrame / NoteField calls go, and the lagged seal result (p7SealedPublished, hadField) comes from the skeleton's lagged seal.
// See link/README.md "Lag bridge until SK-W2".
//
// Render thread only, except Lease() / Source() / Generation() (any thread: atomic loads). Header + .cpp, C++14.
#pragma once

#include "CameraLabelFill.h"

#include <VP-V/core/Indicator.h>
#include <VP-V/core/LaneLease.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/core/ProductSlots.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace CameraLink
{
	class CameraLane
	{
	public:
		// The seal window: frames between a frame's render start and its seal. Also sizes the rings (renderAhead = kSealLag), so
		// a slot is never reclaimed before its frame has been sealed and released.
		static constexpr uint8_t kSealLag = 6;

		struct Sealed
		{
			bool any = false;             // a frame was sealed by this call (`seq` below is the newest one)
			uint64_t seq = 0;
			bool p7Published = false;     // that frame's P7 was lane-published and the seal accepted it
			bool hadField = true;         // the engine had a verified field for that frame (false: a cadence repeat, nothing to fit)
		};

		CameraLane() = default;
		CameraLane(const CameraLane&) = delete;
		CameraLane& operator=(const CameraLane&) = delete;

		// Builds the graph for a source of width x height (the dimensions only size the declarations; nothing is allocated per pixel).
		// `labels` adds the P8 consumer. `indicator` is the stamp source the leases carry (kept alive by them; null = the holders use the
		// stamp they were given). False and Error() when the registry refuses the graph (shown by the caller). Every call (and Clear)
		// starts a new lease generation: a holder of the previous state rebinds and refuses work stamped with the old one.
		bool Build(uint32_t width, uint32_t height, bool labels, std::shared_ptr<VpIndicator::Indicator> indicator = nullptr);
		void Clear();
		bool Built() const { return m_state != nullptr; }
		const std::string& Error() const { return m_error; }

		// The current lease (any thread). bytesProduct = kP7CameraFit adds the P7 bytes ring; any other id none. Without a state: an
		// invalid lease carrying the generation counter (LaneLeaseNewer orders it against the states, as AnalysisLane::Lease does).
		VpSkeleton::LaneLease Lease(VpSkeleton::ProductId bytesProduct) const;
		// A lease source bound to this bridge (the bridge outlives every holder of the source; the leases keep the state alive).
		VpSkeleton::LaneLeaseSource Source(VpSkeleton::ProductId bytesProduct) const;
		uint64_t Generation() const { return m_generation.load(std::memory_order_acquire); }

		// Render thread / tests: the current state's objects (null without one).
		VpSkeleton::SlotTable* Table() const { return m_state ? m_state->table.get() : nullptr; }
		VpSkeleton::CpuBuffers* P7Bytes() const { return m_state ? m_state->p7Bytes.get() : nullptr; }
		bool HasLabels() const { return m_labels; }
		// Ring slots and live buffers of P8 (the texture mirror the label producer is sized by); 0 without P8.
		int LabelRing() const;
		int LabelLive() const;
		uint32_t Width() const { return m_width; }
		uint32_t Height() const { return m_height; }
		void SetLabelFill(CameraLabelFill* fill) { m_fill = fill; }

		// The engine has a verified field for `seq` (render thread, before OnFrame of that or a later frame).
		void NoteField(uint64_t seq);

		// Render thread, at Begin of frame `seq`, after the shared seal. Publishes the input markers of `seq`, then seals and releases
		// every frame up to seq - kSealLag. False = a discontinuity (the sequence went backwards or jumped further than the window): the
		// caller rebuilds the lane; nothing was changed by the call.
		bool OnFrame(uint64_t seq, VpIndicator::Indicator& indicator, Sealed& out);

		uint64_t NextFloor() const { return m_nextToSeal; }   // the first frame not sealed yet (P8 gap work below it is pointless)
		uint64_t MarkersPublished() const { return m_markers; }
		// Input markers that could not be published after the warm-up frame (LaneAncestors failed: an input slot was missing). Each is
		// abandoned (never published with an empty ancestor list), counted here and shown by the extension (Fallbacks "camera_graph").
		uint64_t MarkerFailures() const { return m_markerFailures; }
		uint64_t FramesSealed() const { return m_sealedFrames; }

	private:
		// What a lease holds: the table and everything it needs, alive for as long as any holder keeps it (the bridge drops its own
		// reference at Build / Clear while the fit thread may still be in a publish on the old one).
		struct State
		{
			VpSkeleton::Graph graph;
			std::unique_ptr<VpSkeleton::SlotTable> table;
			std::unique_ptr<VpSkeleton::CpuBuffers> p7Bytes;
			std::shared_ptr<VpIndicator::Indicator> indicator;
			uint64_t generation = 0;
		};

		void PublishMarkers(uint64_t seq, VpIndicator::Indicator& indicator, bool warmUp);
		void SealOne(uint64_t seq, VpIndicator::Indicator& indicator, Sealed& out);

		std::shared_ptr<State> m_state;              // swapped by the render thread (std::atomic_store), read by Lease (std::atomic_load)
		std::atomic<uint64_t> m_generation{ 0 };     // the last generation handed out; bumped at Build and Clear
		CameraLabelFill* m_fill = nullptr;
		std::string m_error;
		uint32_t m_width = 0, m_height = 0;
		bool m_labels = false;
		bool m_started = false;
		uint64_t m_lastSeq = 0;
		uint64_t m_nextToSeal = 0;
		uint64_t m_markers = 0, m_markerFailures = 0, m_sealedFrames = 0;
		std::vector<uint64_t> m_fieldMark;   // seq + 1 at index seq % size: the engine had a field for seq
	};
}
