// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-3): the record pool parallel to P7's SlotSet, and the POD codec for the P7 slot bytes.
// The slot holds a fixed, small CameraRecordPod (declared 1 KB; the encoded form is about 330 B); the heavy parts (the
// per-block arrays and the CPU field copy) are shared_ptrs kept here, indexed by seq mod ring like the SlotSet. The
// producer writes an entry only while it holds the claim for that seq; readers use an entry only for a seq whose slot
// they have pinned / sealed. Pure, header-only, C++14, deterministic.
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
	constexpr size_t kRecordPodMaxBytes = 1024;   // the declared P7 size (core/Products.h p7.size.fixedBytes)
	constexpr uint32_t kRecordPodMagic = 0x43524431u;   // "CRD1"

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
	}

	// Encodes everything of the record except `blocks` and `objects` (the objects stay off in K1; their count is stored so
	// a decode can tell). Returns the byte count, 0 when the buffer is too small.
	inline size_t EncodeRecordPod(const CameraModel::CameraMotion& m, uint8_t* out, size_t cap)
	{
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
		return w.ok ? w.n : 0;
	}

	// Decodes into `m` (blocks left null). False on a bad magic or a short buffer.
	inline bool DecodeRecordPod(const uint8_t* in, size_t bytes, CameraModel::CameraMotion& m)
	{
		detail::Reader r(in, bytes);
		if (r.Get<uint32_t>() != kRecordPodMagic)
			return false;
		CameraModel::CameraMotion x;
		x.seq = r.Get<uint64_t>(); x.pairFrom = r.Get<uint64_t>(); x.pairTo = r.Get<uint64_t>(); x.pairSeconds = r.Get<double>();
		x.resyncGeneration = r.Get<uint64_t>(); x.rebaseGeneration = r.Get<uint64_t>();
		x.valid = r.Get<uint8_t>() != 0;
		x.params.type = static_cast<CameraModel::ModelType>(r.Get<uint8_t>());
		x.reason = static_cast<CameraModel::Reason>(r.Get<uint8_t>());
		const uint8_t flags = r.Get<uint8_t>();
		x.seededFromNext = (flags & 1) != 0; x.bridged = (flags & 2) != 0; x.inFade = (flags & 4) != 0;
		x.smoothed = (flags & 8) != 0; x.labelsOnGpu = (flags & 16) != 0;
		for (int i = 0; i < 9; ++i) x.params.h[i] = r.Get<double>();
		x.smoothedParams.type = static_cast<CameraModel::ModelType>(r.Get<uint8_t>());
		for (int i = 0; i < 9; ++i) x.smoothedParams.h[i] = r.Get<double>();
		x.rates.logScale = r.Get<double>(); x.rates.angle = r.Get<double>(); x.rates.tx = r.Get<double>(); x.rates.ty = r.Get<double>();
		x.confidence = r.Get<double>();
		CameraModel::Signals& s = x.signals;
		s.inlierShare = r.Get<double>(); s.precision = r.Get<double>(); s.coverage = r.Get<double>(); s.temporal = r.Get<double>();
		s.conditioning = r.Get<double>(); s.engineAgreement = r.Get<double>(); s.inverse = r.Get<double>();
		s.ambiguity = r.Get<double>(); s.logit = r.Get<double>();
		const uint8_t sf = r.Get<uint8_t>();
		s.haveBackward = (sf & 1) != 0; s.haveEngine = (sf & 2) != 0;
		x.fadeGain = r.Get<double>(); x.fadeOffset = r.Get<double>();
		x.samples = r.Get<uint32_t>(); x.hypotheses = r.Get<uint32_t>(); x.costMicros = r.Get<int64_t>();
		x.picture.left = r.Get<int32_t>(); x.picture.top = r.Get<int32_t>(); x.picture.right = r.Get<int32_t>(); x.picture.bottom = r.Get<int32_t>();
		(void)r.Get<uint32_t>();
		if (!r.ok)
			return false;
		m = x;
		return true;
	}

	// One pool entry: the record (with its BlockData) and the CPU field copy it was fitted from.
	struct PoolEntry
	{
		uint64_t seq = ~uint64_t(0);
		std::shared_ptr<const CameraModel::CameraMotion> record;
		std::shared_ptr<const FieldCopy> field;
	};

	class CameraRecordPool
	{
	public:
		explicit CameraRecordPool(size_t ring) : m_entries(ring == 0 ? 1 : ring) {}

		size_t Ring() const { return m_entries.size(); }

		// Producer (holding the claim for `seq`). Replaces the previous tenant of the slot.
		void Put(uint64_t seq, std::shared_ptr<const CameraModel::CameraMotion> record, std::shared_ptr<const FieldCopy> field)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			PoolEntry& e = m_entries[seq % m_entries.size()];
			e.seq = seq;
			e.record = std::move(record);
			e.field = std::move(field);
		}

		// Reader. False when the slot holds another frame (or nothing).
		bool Get(uint64_t seq, PoolEntry& out) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			const PoolEntry& e = m_entries[seq % m_entries.size()];
			if (e.seq != seq || !e.record)
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
