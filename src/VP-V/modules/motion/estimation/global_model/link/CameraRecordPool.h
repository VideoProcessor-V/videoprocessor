// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-3): the record pool parallel to P7's SlotSet, and the POD codec for the P7 slot bytes.
// The slot holds a fixed, small CameraRecordPod (declared 1 KB; the encoded CRD2 form is 595 B, CRD1 legacy is 366 B); the heavy parts (the
// per-block arrays and the CPU field copy) are shared_ptrs kept here, indexed by seq mod ring like the SlotSet. The
// producer writes an entry only while it holds the claim for that seq; readers use an entry only for a seq whose slot
// they have pinned / sealed. Pure, C++14, deterministic (DecodeRecordPod defined in CameraLinkCore.cpp).
#pragma once

#include "FieldCopy.h"

#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace CameraLink
{
	constexpr size_t kRecordPodMaxBytes = 1024;          // the declared P7 size (core/Products.h p7.size.fixedBytes)
	constexpr uint32_t kRecordPodMagicV1 = 0x43524431u;  // "CRD1" (legacy v1 payload; still decoded cleanly)
	constexpr uint32_t kRecordPodMagic = 0x43524432u;    // "CRD2" (GEM-137: carries verdict/layer flags, finalRefineCells, dyn/water/static/two-layer signals)

	namespace detail
	{
		struct Writer
		{
			uint8_t* p; size_t cap; size_t n = 0; bool ok = true;
			Writer(uint8_t* b, size_t c) : p(b), cap(c) {}
			template <class T> void Put(const T& v)
			{
				if (n + sizeof(T) > cap) { ok = false; return; }
				std::memcpy(p + n, &v, sizeof(T));
				n += sizeof(T);
			}
		};
		struct Reader
		{
			const uint8_t* p; size_t cap; size_t n = 0; bool ok = true;
			Reader(const uint8_t* b, size_t c) : p(b), cap(c) {}
			template <class T> T Get()
			{
				T v{};
				if (n + sizeof(T) > cap) { ok = false; return v; }
				std::memcpy(&v, p + n, sizeof(T));
				n += sizeof(T);
				return v;
			}
		};
		inline void PutFadeFields(Writer& w, const CameraModel::CameraMotion& m) { w.Put(m.fadeGain); w.Put(m.fadeOffset); }
		inline void GetFadeFields(Reader& r, CameraModel::CameraMotion& x) { x.fadeGain = r.Get<double>(); x.fadeOffset = r.Get<double>(); }
	}

	// Encodes everything of the record except `blocks` and `objects` (the objects stay off in K1; their count is stored so
	// a decode can tell). Writes CRD2 (v1 prefix at identical byte offsets + v2 tail). Returns the byte count, 0 when the
	// buffer is null or too small.
	inline size_t EncodeRecordPod(const CameraModel::CameraMotion& m, uint8_t* out, size_t cap)
	{
		if (!out || cap == 0)
			return 0;
		detail::Writer w(out, cap < kRecordPodMaxBytes ? cap : kRecordPodMaxBytes);
		w.Put(kRecordPodMagic);
		w.Put(m.seq); w.Put(m.pairFrom); w.Put(m.pairTo); w.Put(m.pairSeconds);
		w.Put(m.resyncGeneration); w.Put(m.rebaseGeneration);
		w.Put(uint8_t(m.valid)); w.Put(uint8_t(m.params.type)); w.Put(uint8_t(m.reason));
		w.Put(uint8_t(m.seededFromNext | (m.bridged << 1) | (m.inFade << 2) | (m.smoothed << 3) | (m.labelsOnGpu << 4)));
		for (int i = 0; i < 9; ++i) w.Put(m.params.h[i]);
		w.Put(uint8_t(m.smoothedParams.type));
		for (int i = 0; i < 9; ++i) w.Put(m.smoothedParams.h[i]);
		w.Put(m.rates.logScale); w.Put(m.rates.angle); w.Put(m.rates.tx); w.Put(m.rates.ty);
		w.Put(m.confidence);
		const CameraModel::Signals& s = m.signals;
		w.Put(s.inlierShare); w.Put(s.precision); w.Put(s.coverage); w.Put(s.temporal); w.Put(s.conditioning);
		w.Put(s.engineAgreement); w.Put(s.inverse); w.Put(s.ambiguity); w.Put(s.logit);
		w.Put(uint8_t(s.haveBackward | (s.haveEngine << 1)));
		w.Put(m.fadeGain); w.Put(m.fadeOffset);
		w.Put(m.samples); w.Put(m.hypotheses); w.Put(m.costMicros);
		w.Put(int32_t(m.picture.left)); w.Put(int32_t(m.picture.top)); w.Put(int32_t(m.picture.right)); w.Put(int32_t(m.picture.bottom));
		w.Put(uint32_t(m.objects.size()));
		// CRD2 tail (GEM-137): verdict/layer flags, full-resolution refine count, dynamic-texture / water / static / two-layer signals and object layer.
		w.Put(m.verdictFlags);
		w.Put(m.layerFlags);
		w.Put(m.finalRefineCells);
		w.Put(s.dynShare); w.Put(s.dynTemporal); w.Put(s.dynSpatial); w.Put(s.dynField);
		w.Put(s.waterExcludedShare);
		w.Put(s.staticRing); w.Put(s.staticFit);
		w.Put(s.border); w.Put(s.continuity);
		w.Put(m.layerRisk);
		w.Put(uint8_t(m.haveObjectLayer ? 1 : 0));
		w.Put(m.objectLayer.id);
		w.Put(uint8_t(m.objectLayer.type));
		for (int i = 0; i < 9; ++i) w.Put(m.objectLayer.h[i]);
		w.Put(m.objectLayer.blocks);
		w.Put(m.objectLayer.cx); w.Put(m.objectLayer.cy);
		// CE-WATER / C4 diagnostics tail + fit absThresholdPx (appended to CRD2; total 603 B <= 1024 B).
		w.Put(s.dynCurvature); w.Put(s.dynRhoTemp);
		w.Put(s.rawShare); w.Put(s.shearEnergy); w.Put(s.dipoleTB); w.Put(s.dipoleLR);
		const double absThrPx = (m.blocks && m.blocks->absThresholdPx > 0.0f)
			? static_cast<double>(m.blocks->absThresholdPx) : m.absThresholdPx;
		w.Put(absThrPx);
		return w.ok ? w.n : 0;
	}

	// The last enumerators of the two enums the slot stores as bytes (CameraMotion.h): a stored byte above them is not a
	// value of the enum. Keep these in step when an enumerator is appended.
	constexpr uint8_t kRecordPodLastModelType = uint8_t(CameraModel::ModelType::Homography);
	constexpr uint8_t kRecordPodLastReason = uint8_t(CameraModel::Reason::Disabled);

	// Decodes CRD2 or legacy CRD1 into `m` (blocks left null; CRD1 leaves v2 fields at CameraMotion defaults).
	// False on a null/short buffer, bad magic, or a ModelType / Reason byte outside its enum (the target is then untouched).
	bool DecodeRecordPod(const uint8_t* in, size_t bytes, CameraModel::CameraMotion& m);

	// What the pool keeps of the field a record was fitted from: its PROVENANCE only (origin and the P2 epoch the staleness checks read).
	// The field's content is NOT kept here: it lives in the declared product p7f_camera_fit_field (CameraFitFieldPod.h).
	struct FieldProvenance
	{
		bool have = false;
		FieldOrigin origin = FieldOrigin::Synthetic;
		bool haveP2 = false;
		uint64_t p2Epoch = 0;
	};
	inline FieldProvenance ProvenanceOf(const FieldCopy* f)
	{
		FieldProvenance p;
		if (f)
		{
			p.have = true;
			p.origin = f->origin;
			p.haveP2 = f->haveP2;
			p.p2Epoch = f->p2Epoch;
		}
		return p;
	}

	// One pool entry: the record (with its BlockData) and the provenance of the field it was fitted from.
	struct PoolEntry
	{
		uint64_t seq = ~uint64_t(0);
		std::shared_ptr<const CameraModel::CameraMotion> record;
		FieldProvenance field;
		bool committed = false;   // set only AFTER the slot publish succeeded: a late / unpublished record is never served
		// AHEAD FIT: the Indicator stamp honoured at the publish (read after the pins, AheadLane provenance). The consumers re-check it
		// against the frame's current stamp (CameraLinkCore::SealedRecord): a resync after the publish rejects the record there too.
		bool haveHonoured = false;
		VpIndicator::Stamp honoured;
		bool onTime = false;      // published into the on-time target (its Indicator judges the stamp), else the lag bridge
	};

	class CameraRecordPool
	{
	public:
		explicit CameraRecordPool(size_t ring) : m_entries(ring == 0 ? 1 : ring) {}

		size_t Ring() const { return m_entries.size(); }

		// Producer (holding the claim for `seq`). Replaces the previous tenant of the slot.
		void Put(uint64_t seq, std::shared_ptr<const CameraModel::CameraMotion> record, const FieldProvenance& field,
			const VpIndicator::Stamp* honoured = nullptr, bool onTime = false)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			PoolEntry& e = m_entries[seq % m_entries.size()];
			e.seq = seq;
			e.record = std::move(record);
			e.field = field;
			e.committed = false;
			e.haveHonoured = honoured != nullptr;
			e.honoured = honoured ? *honoured : VpIndicator::Stamp();
			e.onTime = onTime;
		}

		// Producer: the slot publish succeeded -> readers may now see the entry.
		void Commit(uint64_t seq)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			PoolEntry& e = m_entries[seq % m_entries.size()];
			if (e.seq == seq)
				e.committed = true;
		}

		// Reader. False when the slot holds another frame (or nothing).
		// includeUncommitted: also reads a Published-but-not-yet-Committed slot (a worker between Publish and Commit). It is for callers
		// whose precondition is that the frame is already Published/Sealed (the seal observer, FitEntryOf); the default stays committed-only.
		bool Get(uint64_t seq, PoolEntry& out, bool includeUncommitted = false) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const PoolEntry& e = m_entries[seq % m_entries.size()];
			if (e.seq != seq || !e.record || (!e.committed && !includeUncommitted))
				return false;
			out = e;
			return true;
		}

		// Producer: drop the entry of `seq` (its publish was late: the record must never be served).
		void Erase(uint64_t seq)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			PoolEntry& e = m_entries[seq % m_entries.size()];
			if (e.seq == seq)
				e = PoolEntry();
		}

		void Clear()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (PoolEntry& e : m_entries)
				e = PoolEntry();
		}

	private:
		mutable std::mutex m_mutex;
		std::vector<PoolEntry> m_entries;
	};
}
