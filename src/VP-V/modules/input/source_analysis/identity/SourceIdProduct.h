// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P14, the source identity product (doc 61 section 2.1, decision D-SID1): the HELD-STATE record the source-resolution worker
// publishes on each evaluation of the consensus. This header holds the unit version, the record (a ~256 B POD with an explicit
// byte codec), the held-state holder consumers read, and the render-side rebase applier.
//
// Held state, no live fill (D-SID1): the consensus is hysteretic (3 s dwell), so recomputing it on the render thread would
// differ from the worker's answer. A consumer reads the latest SEALED record with seq <= N; a record older than its maximum age
// (derived from the settings, never fixed) is stale and every consumer falls back to the classical value, visibly
// (SourceQualityView.h). Header-only, standard library only, C++14, no clock: time is source time passed in.
#include <VP-V/modules/input/source_analysis/identity/SourceIdConsensus.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace SourceIdentity
{
	// Bump on ANY change of the record layout or of the law behind a field; core/ProductCatalog.h holds the expected number.
	constexpr uint32_t kUnitVersion = 1;
	constexpr size_t kRecordBytes = 256;

	// Why a field holds its value, as a compact code (FieldResult::reason strings, stable text via ReasonText).
	enum class ReasonCode : uint8_t
	{
		NoVotes = 0, Agrees, Changed, HeldDisagreement, HeldWeak, HeldTooFew, HeldDwell, HeldMinHold, HeldStale, Veto, Count
	};

	inline const char* ReasonText(ReasonCode r)
	{
		switch (r)
		{
		case ReasonCode::NoVotes: return "no votes";
		case ReasonCode::Agrees: return "agrees";
		case ReasonCode::Changed: return "changed";
		case ReasonCode::HeldDisagreement: return "held: disagreement";
		case ReasonCode::HeldWeak: return "held: weak evidence";
		case ReasonCode::HeldTooFew: return "held: too few voters";
		case ReasonCode::HeldDwell: return "held: dwell";
		case ReasonCode::HeldMinHold: return "held: min hold";
		case ReasonCode::HeldStale: return "held: stale";
		case ReasonCode::Veto: return "safety veto";
		default: return "?";
		}
	}

	inline ReasonCode ReasonOf(const std::string& s)
	{
		for (int i = 0; i < static_cast<int>(ReasonCode::Count); ++i)
		{
			const std::string t = ReasonText(static_cast<ReasonCode>(i));
			if (s == t || (i == static_cast<int>(ReasonCode::Veto) && s.compare(0, 11, "safety veto") == 0))
				return static_cast<ReasonCode>(i);
		}
		return ReasonCode::NoVotes;
	}

	enum RecordFlags : uint16_t
	{
		kFlagDropEventSeen = 1u << 0,   // an ABR Drop event is the newest event
		kFlagNoticeFired = 1u << 1,     // the one-time notice fired for this title
		kFlagConsensusOn = 1u << 2,     // the worker ran with source_id_consensus on when it published
		kFlagReading = 1u << 3          // the classical monitor had a reading
	};

	// What the source is (the latched consensus state) plus the evidence behind it. Logical form; Pack/Unpack are the codec.
	struct Record
	{
		uint32_t unitVersion = kUnitVersion;
		uint16_t flags = 0;
		uint64_t seq = 0;                 // newest evidence sequence behind this evaluation
		int64_t timeUs = 0;               // source time of the evaluation
		uint64_t consensusGen = 0;        // bumps whenever any field changed
		uint64_t streamKey = 0;           // the Indicator's FULL/era counter (title)
		uint64_t publishSerial = 0;       // 1, 2, ... per Publish since the holder's reset
		int64_t maxAgeUs = 0;             // older than this = stale
		float ratioH = 1.0f, ratioV = 1.0f;
		int32_t kernel = -1;
		uint8_t codec = 0;                // CodecClass
		int8_t tier = -1;                 // quality tier, -1 unknown
		float quality = 1.0f;             // the quality the tier was made from (0..1)
		float level = 1.0f;               // event-anchored relative stream level (1 until an ABR event)
		uint8_t latched = 0;              // bit per Field: changed at least once since the last discontinuity
		uint8_t reason[kFieldCount] = {};
		float confidence[kFieldCount] = {};   // support of the candidate / current group per field
		// The newest ABR event.
		uint8_t abrDirection = 0;         // 0 none, 1 drop, 2 rise
		int64_t abrTimeUs = 0;
		float abrMagnitude = 0.0f, abrRatioStep = 0.0f, abrConfidence = 0.0f;
		uint64_t noticeSerial = 0;        // bumps when the one-time notice fires
		// Rebase requests the consensus made; the render side applies each generation exactly once (RebaseApplier).
		uint32_t rebaseGen[2] = {};
		uint8_t rebaseCause[2] = {};      // 0 none, 1 ratio, 2 kernel, 3 veto
		uint32_t discontinuities = 0;

		bool Latched(Field f) const { return (latched & (1u << static_cast<unsigned>(f))) != 0; }
		Hypothesis Value() const
		{
			Hypothesis h;
			h.ratioH = ratioH;
			h.ratioV = ratioV;
			h.kernel = kernel;
			h.codec = static_cast<CodecClass>(codec);
			h.qualityTier = tier;
			return h;
		}
	};

	inline uint8_t RebaseCauseCode(const std::string& c)
	{
		return c == "ratio" ? 1 : c == "kernel" ? 2 : c == "veto" ? 3 : 0;
	}
	inline const char* RebaseCauseText(uint8_t c)
	{
		return c == 1 ? "ratio" : c == 2 ? "kernel" : c == 3 ? "veto" : "none";
	}

	// ---- byte codec (little endian, fixed layout; kRecordBytes) ----------------------------------------------------
	namespace detail
	{
		struct Writer
		{
			uint8_t* p;
			size_t n = 0;
			template <class T> void Put(T v)
			{
				uint64_t u = 0;
				std::memcpy(&u, &v, sizeof(T));
				for (size_t i = 0; i < sizeof(T); ++i)
					p[n++] = static_cast<uint8_t>((u >> (8 * i)) & 0xFFu);
			}
		};
		struct Reader
		{
			const uint8_t* p;
			size_t n = 0;
			template <class T> T Get()
			{
				uint64_t u = 0;
				for (size_t i = 0; i < sizeof(T); ++i)
					u |= static_cast<uint64_t>(p[n++]) << (8 * i);
				T v;
				std::memcpy(&v, &u, sizeof(T));
				return v;
			}
		};
	}

	struct Packed
	{
		uint8_t bytes[kRecordBytes];
	};

	inline Packed Pack(const Record& r)
	{
		Packed out;
		std::memset(out.bytes, 0, sizeof(out.bytes));
		detail::Writer w{ out.bytes };
		w.Put(r.unitVersion); w.Put(r.flags);
		w.Put(r.seq); w.Put(r.timeUs); w.Put(r.consensusGen); w.Put(r.streamKey); w.Put(r.publishSerial); w.Put(r.maxAgeUs);
		w.Put(r.ratioH); w.Put(r.ratioV); w.Put(r.kernel); w.Put(r.codec); w.Put(r.tier); w.Put(r.quality); w.Put(r.level);
		w.Put(r.latched);
		for (int i = 0; i < kFieldCount; ++i) w.Put(r.reason[i]);
		for (int i = 0; i < kFieldCount; ++i) w.Put(r.confidence[i]);
		w.Put(r.abrDirection); w.Put(r.abrTimeUs); w.Put(r.abrMagnitude); w.Put(r.abrRatioStep); w.Put(r.abrConfidence);
		w.Put(r.noticeSerial);
		for (int i = 0; i < 2; ++i) w.Put(r.rebaseGen[i]);
		for (int i = 0; i < 2; ++i) w.Put(r.rebaseCause[i]);
		w.Put(r.discontinuities);
		return out;
	}

	// False when the bytes carry another unit version (the reader falls back, visibly).
	inline bool Unpack(const Packed& in, Record& r)
	{
		detail::Reader rd{ in.bytes };
		Record t;
		t.unitVersion = rd.Get<uint32_t>(); t.flags = rd.Get<uint16_t>();
		t.seq = rd.Get<uint64_t>(); t.timeUs = rd.Get<int64_t>(); t.consensusGen = rd.Get<uint64_t>();
		t.streamKey = rd.Get<uint64_t>(); t.publishSerial = rd.Get<uint64_t>(); t.maxAgeUs = rd.Get<int64_t>();
		t.ratioH = rd.Get<float>(); t.ratioV = rd.Get<float>(); t.kernel = rd.Get<int32_t>(); t.codec = rd.Get<uint8_t>();
		t.tier = rd.Get<int8_t>(); t.quality = rd.Get<float>(); t.level = rd.Get<float>();
		t.latched = rd.Get<uint8_t>();
		for (int i = 0; i < kFieldCount; ++i) t.reason[i] = rd.Get<uint8_t>();
		for (int i = 0; i < kFieldCount; ++i) t.confidence[i] = rd.Get<float>();
		t.abrDirection = rd.Get<uint8_t>(); t.abrTimeUs = rd.Get<int64_t>(); t.abrMagnitude = rd.Get<float>();
		t.abrRatioStep = rd.Get<float>(); t.abrConfidence = rd.Get<float>();
		t.noticeSerial = rd.Get<uint64_t>();
		for (int i = 0; i < 2; ++i) t.rebaseGen[i] = rd.Get<uint32_t>();
		for (int i = 0; i < 2; ++i) t.rebaseCause[i] = rd.Get<uint8_t>();
		t.discontinuities = rd.Get<uint32_t>();
		if (t.unitVersion != kUnitVersion)
			return false;
		r = t;
		return true;
	}

	static_assert(sizeof(Packed) == kRecordBytes, "P14 record bytes");
	// The packed fields must fit (a field added to the codec without checking the size would silently truncate).
	constexpr size_t kPackedFieldBytes = 4 + 2 + 8 * 6 + 4 * 2 + 4 + 1 + 1 + 4 + 4 + 1 + kFieldCount + 4 * kFieldCount +
		1 + 8 + 4 * 3 + 8 + 4 * 2 + 2 + 4;
	static_assert(kPackedFieldBytes <= kRecordBytes, "P14 record fields exceed the declared 256 bytes");

	// ---- the held-state holder -------------------------------------------------------------------------------------
	// The worker publishes (claim, write, stamp last = publishSerial, publish); the render thread reads the latest record
	// with seq <= N. A small ring keeps the last kRing records so a read for an earlier frame (look-ahead, render-ahead)
	// still finds the record that was current for it. Mutex-guarded: the record is ~256 B, read once per frame.
	class HeldState
	{
	public:
		static constexpr size_t kRing = 8;

		void Reset()
		{
			std::lock_guard<std::mutex> g(m_);
			count_ = 0;
			head_ = 0;
			serial_ = 0;
		}

		// Worker thread. Stamps the serial last and returns it.
		uint64_t Publish(Record r)
		{
			std::lock_guard<std::mutex> g(m_);
			r.publishSerial = ++serial_;
			ring_[head_] = pack_(r);
			head_ = (head_ + 1) % kRing;
			if (count_ < kRing) ++count_;
			return r.publishSerial;
		}

		// Any thread: the newest record whose evidence seq is <= `seq` (0 = the newest of all). False: nothing published
		// yet, or nothing that old (the caller falls back to the classical value).
		bool ReadLatest(uint64_t seq, Record& out) const
		{
			std::lock_guard<std::mutex> g(m_);
			for (size_t i = 0; i < count_; ++i)
			{
				const size_t idx = (head_ + kRing - 1 - i) % kRing;
				Record r;
				if (!Unpack(ring_[idx], r))
					continue;
				if (seq == 0 || r.seq <= seq)
				{
					out = r;
					return true;
				}
			}
			return false;
		}

		uint64_t Serial() const
		{
			std::lock_guard<std::mutex> g(m_);
			return serial_;
		}

	private:
		static Packed pack_(const Record& r) { return Pack(r); }
		mutable std::mutex m_;
		Packed ring_[kRing];
		size_t count_ = 0, head_ = 0;
		uint64_t serial_ = 0;
	};

	// ---- the render-side rebase applier ----------------------------------------------------------------------------
	// A sealed record carries the consensus's rebase requests as per-axis generations. Poll applies each new generation to the
	// IndicatorPort exactly once (also when the same record is polled again, or when a newer record carries the same generation).
	class RebaseApplier
	{
	public:
		// Returns the number of AdvanceRebase calls made. A record from another stream key resets the applier (a new title).
		int Poll(const Record& r, IndicatorPort& port)
		{
			// A new title (stream key) or a discontinuity: the worker reset its generations to 0, so this stream's requests start
			// again from 0. Requests of the old stream are never replayed (the Indicator's FULL already re-based both axes).
			if (r.streamKey != streamKey_ || r.discontinuities != discontinuities_)
			{
				streamKey_ = r.streamKey;
				discontinuities_ = r.discontinuities;
				applied_[0] = applied_[1] = 0;
			}
			int calls = 0;
			for (int axis = 0; axis < 2; ++axis)
				if (r.rebaseGen[axis] > applied_[axis])
				{
					applied_[axis] = r.rebaseGen[axis];
					port.AdvanceRebase(axis, r.seq, RebaseCauseText(r.rebaseCause[axis]));
					++calls;
				}
			return calls;
		}
		uint32_t Applied(int axis) const { return applied_[axis]; }

	private:
		uint64_t streamKey_ = ~0ull;
		uint32_t discontinuities_ = ~0u;
		uint32_t applied_[2] = { 0, 0 };
	};
}
