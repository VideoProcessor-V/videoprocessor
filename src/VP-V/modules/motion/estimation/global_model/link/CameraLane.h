// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (camera-live wiring): the camera's own product table, until P7 moves into the skeleton lane (K1-9 / SK-W).
//
// WHY IT EXISTS. P7 (camera fit) is a skeleton product: the fit worker claims P7(N) in a SlotSet, pins its ancestors P3 / P4 / P2
// (CameraFitWorker::PublishFit -> SlotTable::LaneAncestors), publishes, and the seal accepts or rejects it by the stamp. The skeleton
// lane (SkeletonModule) carries only its report consumer today: no graph in a renderer holds P7, P2 or P3, and nothing publishes those
// as slots (the vector engine keeps its field as a texture; P4's scene facts reach the frame-analysis store, not a P4 slot the
// camera could pin). So the camera builds its OWN graph with the same registry and the same SlotTable / SlotSet / CpuBuffers code
// (SkeletonProducts + the camera consumers), and acts as the producer of the INPUT slots:
//   * input markers: for every frame the camera publishes empty-content slots for P3 / P4 / P2m / P1 (every node but P7..P9) with the
//     recorded ancestors and the honoured stamp, exactly as a lane producer would. The CONTENT the fit reads is not in those slots: P3's
//     is the engine's verified field (read back by the FieldReadbackRing into a FieldCopy), P4's is the frame-analysis store's scene
//     record (PairFactsProvider). The markers are what P7's ancestry (cgen, stamp) rests on, so a stamp that moves between the
//     marker and the seal (a resync event, a slip) rejects the record, as it would for any product.
//   * the seal runs kSealLag frames BEHIND the render: P7(N) is fitted from a field the engine made while rendering N, read back a
//     frame or two later, so it cannot exist at the seal of N. SlotTable::Resolve seals the frame that left the window, with the
//     Indicator's stamp OF THAT FRAME (StampOf(s)), and ReleaseAfter retires it (the N2 rule). The record chain, the guidance lag d
//     and the guidance gate read records by seq (ICameraLink::CausalRecord / PreviousRecordSeq), never "the record of the frame being
//     rendered", so the lag is invisible to them.
//   * P8 (camera labels) lives in the same table when labels are wanted: its fill (CameraLabelFill) runs as the P8 gap work and as the
//     seal's live fill, with CPU labels uploaded to a texture (CameraLabelProducer, upload mode).
// P9 (smoothed camera) is declared by no consumer here: its producer (K9-1 CameraSmoother) does not exist yet.
//
// Render thread only (the table is also read by the camera worker, which is what SlotSet is for). Header + .cpp, C++14.
#pragma once

#include "CameraLabelFill.h"

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ProductGraph.h>
#include <VP-V/core/ProductSlots.h>

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
		// `labels` adds the P8 consumer. False and Error() when the registry refuses the graph (shown by the caller).
		bool Build(uint32_t width, uint32_t height, bool labels);
		void Clear();
		bool Built() const { return m_table != nullptr; }
		const std::string& Error() const { return m_error; }

		VpSkeleton::SlotTable* Table() { return m_table.get(); }
		VpSkeleton::CpuBuffers* P7Bytes() { return m_p7Bytes.get(); }
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
		uint64_t FramesSealed() const { return m_sealedFrames; }

	private:
		void PublishMarkers(uint64_t seq, VpIndicator::Indicator& indicator);
		void SealOne(uint64_t seq, VpIndicator::Indicator& indicator, Sealed& out);

		VpSkeleton::Graph m_graph;
		std::unique_ptr<VpSkeleton::SlotTable> m_table;
		std::unique_ptr<VpSkeleton::CpuBuffers> m_p7Bytes;
		CameraLabelFill* m_fill = nullptr;
		std::string m_error;
		uint32_t m_width = 0, m_height = 0;
		bool m_labels = false;
		bool m_started = false;
		uint64_t m_lastSeq = 0;
		uint64_t m_nextToSeal = 0;
		uint64_t m_markers = 0, m_sealedFrames = 0;
		std::vector<uint64_t> m_fieldMark;   // seq + 1 at index seq % size: the engine had a field for seq
	};
}
