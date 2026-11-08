// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P14g `source_geometry` (skeleton product id 29; doc 78 section 6, request of the source-identity lead, wave S2): the RESERVATION of the
// held-state source geometry record. This header holds what the core and every reader need before the estimator exists: the unit version,
// the size bound and byte order of the record, the staleness rule, the provisional readers, the ABSENT placeholder producer and the one
// read API with its visible fallback. The record's fields (per axis: measured and applied state, n, N, ratio, phase, kernel family and
// parameters, kernel confidence, sharpening, double scaling, confidence, LLR, reason bits; the active area; the coded-raster class; the
// reserved chroma chain; geometryGen, appliedGen, evalSeq, evalTimeUs, maxAgeUs) are laid out by the source-identity build (SID-C) inside
// kRecordBytesMax: bump kUnitVersion here AND kExpectedSourceGeometryUnit (core/ProductCatalog.h) in the same commit on ANY change of the
// layout or of the law behind a field. The shared accessor `identity/SourceGeometryView.h` (Latched() / Provisional(), type-separated) is
// that build's file; it reads through Read() below.
//
// HELD STATE, like P14 (SourceIdProduct.h): the source-resolution side is the single writer and publishes on each evaluation; a consumer
// reads the latest SEALED record with seq <= N, i.e. a lag >= 1 read of the frame it renders, never a live fill (the estimator is
// hysteretic: recomputing it on the render thread would differ from the writer's answer). A record older than its maximum age is stale.
// Absent / stale / another unit version = the reader keeps the DELIVERED raster and its classical form, visibly (no silent fallback):
//   quality_pattern, coding_evidence   grid-free evidence ("frame pattern: no source geometry")
//   temporal detail                    the delivered raster (levels 3 / 4 of 68v2)
//   adaptive scaler, chroma stage      today's behaviour, byte-identical
//   status                             "Source identity: classical only (<reason>)"
//
// "Requested": no configuration key asks for the geometry today (the estimator is not built and the SCHEMA lock is not ours), so
// kRequestedToday is false and every Read() is "not requested": no status line, nothing engaged, byte-identical to not reading at all. The
// source-identity build flips the request to its key (source_id_consensus) and replaces the placeholder with its record; the readers do not
// change. Pure functions over plain structs (no clock: `nowUs` is passed in), header-only, C++14, no Windows.
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/Products.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace SourceGeometry
{
	// Bump on ANY change of the record layout or of the law behind a field; core/ProductCatalog.h holds the expected number.
	constexpr uint32_t kUnitVersion = 1;

	// The product id is the core catalogue's (VpSkeleton::kSourceGeometry, core/Products.h): a module header declares no descriptor and no
	// id alias of its own (NoModuleHeaderDefinesItsOwnProductDescriptor); the registry row is core/Products.h AppendSourceGeometryProduct.

	// The record is a fixed little-endian layout of at most this many bytes (the registry declares exactly this figure, system RAM).
	constexpr size_t kRecordBytesMax = VpSkeleton::kSourceGeometryRecordBytes;
	static_assert(kRecordBytesMax == 512, "doc 78 section 6: the record is at most 512 B");

	// Stale after maxAgeUs = kMaxAgeIntervals x the longest evaluation interval (doc 78 section 6). The interval is MEASURED by the writer
	// (never a fixed rate): the writer states the result in its record, a reader only compares. 0 = the interval is not measured yet (the
	// reader says so, see kCauseNotMeasured); the product saturates instead of overflowing.
	constexpr int64_t kMaxAgeIntervals = 2;
	inline int64_t MaxAgeUs(int64_t evaluationIntervalMaxUs)
	{
		if (evaluationIntervalMaxUs <= 0)
			return 0;
		const int64_t top = (std::numeric_limits<int64_t>::max)();
		return evaluationIntervalMaxUs > top / kMaxAgeIntervals ? top : kMaxAgeIntervals * evaluationIntervalMaxUs;
	}

	// |a - b| in microseconds for ANY two int64 values, without signed overflow (unsigned arithmetic is exact: the difference fits uint64).
	inline uint64_t AbsDiffUs(int64_t a, int64_t b)
	{
		return a >= b ? static_cast<uint64_t>(a) - static_cast<uint64_t>(b) : static_cast<uint64_t>(b) - static_cast<uint64_t>(a);
	}

	// ---- the record header: a fixed 8 B in front of the payload -----------------------------------------------------------------
	//   offset 0  u16  (kMagic << 8) | unit version   (the version fits the low byte)
	//   offset 2  u16  the record length in bytes, header included (kHeaderBytes .. kRecordBytesMax)
	//   offset 4  u32  reserved: written 0, ignored on read (room for flags without a version bump of the header)
	// A reader checks the header BEFORE it trusts a byte of the payload: another magic / unit version / a length beyond the buffer or the
	// 512 B bound is a visible cause, never a guess. The payload is at most kPayloadBytesMax.
	constexpr size_t kHeaderBytes = 8;
	constexpr size_t kPayloadBytesMax = kRecordBytesMax - kHeaderBytes;
	constexpr uint8_t kMagic = 0x47;   // 'G'
	static_assert(kUnitVersion <= 0xFFu, "the unit version lives in the low byte of the header's first u16");

	// ---- the byte order of the record: fixed little-endian, independent of the host --------------------------------------------

	inline void PutU16(unsigned char* at, uint16_t v)
	{
		at[0] = static_cast<unsigned char>(v & 0xFFu);
		at[1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
	}
	inline void PutU32(unsigned char* at, uint32_t v)
	{
		for (unsigned i = 0; i < 4; ++i)
			at[i] = static_cast<unsigned char>((v >> (8u * i)) & 0xFFu);
	}
	inline void PutU64(unsigned char* at, uint64_t v)
	{
		for (unsigned i = 0; i < 8; ++i)
			at[i] = static_cast<unsigned char>((v >> (8u * i)) & 0xFFu);
	}
	inline uint16_t GetU16(const unsigned char* at) { return static_cast<uint16_t>(at[0] | (unsigned(at[1]) << 8)); }
	inline uint32_t GetU32(const unsigned char* at)
	{
		uint32_t v = 0;
		for (unsigned i = 0; i < 4; ++i)
			v |= static_cast<uint32_t>(at[i]) << (8u * i);
		return v;
	}
	inline uint64_t GetU64(const unsigned char* at)
	{
		uint64_t v = 0;
		for (unsigned i = 0; i < 8; ++i)
			v |= static_cast<uint64_t>(at[i]) << (8u * i);
		return v;
	}

	// Writes the 8 B header at `at` (kHeaderBytes writable). False (nothing written) for a length outside kHeaderBytes .. kRecordBytesMax.
	inline bool WriteHeader(unsigned char* at, size_t lengthBytes)
	{
		if (lengthBytes < kHeaderBytes || lengthBytes > kRecordBytesMax)
			return false;
		PutU16(at, static_cast<uint16_t>((unsigned(kMagic) << 8) | kUnitVersion));
		PutU16(at + 2, static_cast<uint16_t>(lengthBytes));
		PutU32(at + 4, 0u);
		return true;
	}

	enum class HeaderStatus : uint8_t { Ok, TooShort, BadMagic, UnitVersion, BadLength };
	struct Header
	{
		uint16_t unitVersion = 0;
		uint16_t lengthBytes = 0;
	};
	inline const char* HeaderText(HeaderStatus s)
	{
		switch (s)
		{
		case HeaderStatus::Ok: return "ok";
		case HeaderStatus::TooShort: return "buffer shorter than the 8 B header";
		case HeaderStatus::BadMagic: return "not a source geometry record";
		case HeaderStatus::UnitVersion: return "unit version";
		case HeaderStatus::BadLength: return "record length outside the header .. 512 B bound or beyond the buffer";
		}
		return "?";
	}
	// `available` = the bytes readable at `at`. On Ok `out` holds the version and the length.
	inline HeaderStatus ReadHeader(const unsigned char* at, size_t available, Header* out)
	{
		if (!at || available < kHeaderBytes)
			return HeaderStatus::TooShort;
		const uint16_t first = GetU16(at);
		if ((first >> 8) != kMagic)
			return HeaderStatus::BadMagic;
		const uint16_t version = static_cast<uint16_t>(first & 0xFFu);
		if (version != kUnitVersion)
			return HeaderStatus::UnitVersion;
		const uint16_t length = GetU16(at + 2);
		if (length < kHeaderBytes || length > kRecordBytesMax || length > available)
			return HeaderStatus::BadLength;
		if (out)
		{
			out->unitVersion = version;
			out->lengthBytes = length;
		}
		return HeaderStatus::Ok;
	}

	// ---- the field budget (doc 78 section 6) --------------------------------------------------------------------------------------
	// The CEILING the source-identity layout must stay within, one line per field of the doc 78 list with its byte width and how many
	// times it occurs: per axis (horizontal, vertical) and per side (measured, applied) the state, n, N, ratio, phase, kernel family and two
	// parameters, kernel confidence, sharpening, double scaling, confidence, LLR and reason bits; per axis the effective bandwidth; the active
	// area; the coded-raster class; the reserved chroma chain (two planes); the generations and timing. NOT the layout (SID-C lays the bytes
	// out and bumps kUnitVersion with any change); a test pins that header + budget fits kRecordBytesMax with room to grow.
	struct FieldBudget
	{
		const char* name;
		uint16_t bytes;   // one occurrence
		uint8_t count;    // occurrences (axis x side = 4, per axis = 2, ...)
	};
	inline const FieldBudget* FieldBudgets(size_t* count)
	{
		static const FieldBudget kFields[] = {
			{ "state (per axis and side)", 1, 4 },
			{ "n", 2, 4 },
			{ "N", 2, 4 },
			{ "ratio", 4, 4 },
			{ "phase", 4, 4 },
			{ "kernel family", 1, 4 },
			{ "kernel parameters (2 x f32)", 8, 4 },
			{ "kernel confidence", 4, 4 },
			{ "sharpening {present, amount, radius, confidence}", 13, 4 },
			{ "double scaling {present, n1, confidence}", 7, 4 },
			{ "confidence", 4, 4 },
			{ "LLR", 4, 4 },
			{ "reason bits", 4, 4 },
			{ "effective bandwidth per axis", 4, 2 },
			{ "active area {x, y, w, h, confidence}", 12, 1 },
			{ "coded-raster class", 1, 1 },
			{ "chroma chain per plane {origin sampling, player-upsampled, kernel, siting x, siting y, confidence} (reserved)", 9, 2 },
			{ "geometryGen", 8, 1 },
			{ "appliedGen", 8, 1 },
			{ "evalSeq", 8, 1 },
			{ "evalTimeUs", 8, 1 },
			{ "maxAgeUs", 8, 1 },
		};
		if (count)
			*count = sizeof(kFields) / sizeof(kFields[0]);
		return kFields;
	}
	inline size_t FieldBudgetBytes()
	{
		size_t n = 0;
		const FieldBudget* f = FieldBudgets(&n);
		size_t total = 0;
		for (size_t i = 0; i < n; ++i)
			total += size_t(f[i].bytes) * f[i].count;
		return total;
	}

	// ---- the readers --------------------------------------------------------------------------------------------------------------

	// The provisional readers (doc 78 section 6 table; the catalog rows in core/ProductCatalog.h state them, a test compares).
	enum class Consumer : uint8_t { QualityPattern, CodingEvidence, TemporalDetail, AdaptiveScaler, ChromaStage, Status };
	constexpr size_t kConsumerCount = 6;

	inline const char* Name(Consumer c)
	{
		switch (c)
		{
		case Consumer::QualityPattern: return "quality pattern";
		case Consumer::CodingEvidence: return "coding evidence";
		case Consumer::TemporalDetail: return "temporal detail";
		case Consumer::AdaptiveScaler: return "adaptive scaling";
		case Consumer::ChromaStage: return "chroma stage";
		case Consumer::Status: return "status";
		}
		return "?";
	}

	// What the reader does instead when the record cannot be used (its classical form, never nothing).
	inline const char* ClassicalForm(Consumer c)
	{
		switch (c)
		{
		case Consumer::QualityPattern:
		case Consumer::CodingEvidence: return "grid-free evidence (frame pattern: no source geometry)";
		case Consumer::TemporalDetail: return "the delivered raster";
		case Consumer::AdaptiveScaler:
		case Consumer::ChromaStage: return "today's behaviour";
		case Consumer::Status: return "Source identity: classical only";
		}
		return "";
	}

	// What the writer says about the record right now.
	struct Entry
	{
		bool published = false;
		uint32_t unitVersion = 0;   // the unit version that made it
		uint64_t seq = 0;           // the frame the evaluation was sealed with: visible to a reader of frame N only when seq <= N
		int64_t timeUs = 0;         // source time of the evaluation
		int64_t maxAgeUs = 0;       // stale after this age; <= 0 = the writer has not measured its interval yet: never used (kCauseNotMeasured)
	};

	// The reservation's PLACEHOLDER producer: nothing is ever published (absent).
	inline const Entry* PlaceholderEntry()
	{
		static const Entry empty{};
		return &empty;
	}

	// Whether any consumer asks for the geometry. No configuration key exists yet (deferred to the source-identity build), so false.
	constexpr bool kRequestedToday = false;

	// Why the record is not usable: one bit per cause.
	enum : unsigned { kCauseAbsent = 1u, kCauseUnitVersion = 2u, kCauseStale = 4u, kCauseNotYet = 8u, kCauseNoProducer = 16u, kCauseNotMeasured = 32u };

	inline unsigned CauseOf(const Entry* e, uint64_t frameSeq, int64_t nowUs)
	{
		if (!e)
			return kCauseNoProducer;
		if (!e->published)
			return kCauseAbsent;
		if (e->unitVersion != kUnitVersion)
			return kCauseUnitVersion;
		if (e->seq > frameSeq)
			return kCauseNotYet;   // sealed after the frame being rendered: a reader never reads the future
		// The writer states no maximum age until it has MEASURED its evaluation interval: say so plainly (not "stale"), and never use the record.
		if (e->maxAgeUs <= 0)
			return kCauseNotMeasured;
		if (AbsDiffUs(nowUs, e->timeUs) > static_cast<uint64_t>(e->maxAgeUs))
			return kCauseStale;
		return 0;
	}

	inline const char* CauseWord(unsigned cause)
	{
		switch (cause)
		{
		case kCauseAbsent: return "absent";
		case kCauseUnitVersion: return "unit version";
		case kCauseStale: return "stale";
		case kCauseNotYet: return "not sealed yet";
		case kCauseNoProducer: return "no producer";
		case kCauseNotMeasured: return "evaluation interval not measured yet";
		}
		return "";
	}

	struct ReadResult
	{
		bool requested = false;
		bool usable = false;          // the consumer may use the record
		unsigned cause = 0;           // kCause*; 0 unless requested && !usable
		std::string reason;           // "" unless the classical form stands in for a requested record
		// Engaged exactly when requested && !usable: id "product.source_geometry" (FallbackCatalog product entry) with the reason, which
		// names the consumer and the cause.
		FallbackCatalog::Decision fallback;
	};

	// `entry` may be null (no producer at all). Not requested: nothing is engaged, no string is built (byte-identical to not reading).
	inline ReadResult Read(Consumer consumer, const Entry* entry, bool requested, uint64_t frameSeq, int64_t nowUs)
	{
		ReadResult r;
		r.requested = requested;
		if (!requested)
			return r;
		r.cause = CauseOf(entry, frameSeq, nowUs);
		if (r.cause == 0)
		{
			r.usable = true;
			return r;
		}
		r.reason = std::string(Name(consumer)) + ": source geometry " + CauseWord(r.cause) + ", " + ClassicalForm(consumer);
		r.fallback = FallbackCatalog::ProductFallback("product.source_geometry", r.reason);
		return r;
	}

	// The read of a consumer in this build: the placeholder producer, nothing requested. Byte-identical to not reading at all.
	inline ReadResult ReadToday(Consumer consumer) { return Read(consumer, PlaceholderEntry(), kRequestedToday, 0, 0); }

	// The ONE decision of `product.source_geometry` over the reads of every consumer (one id, so one consumer's recovery cannot release
	// another's entry): engaged while any requested read is not usable, the reason naming each consumer. Released otherwise.
	inline FallbackCatalog::Decision Combine(const ReadResult* reads, size_t count)
	{
		std::string why;
		for (size_t i = 0; i < count; ++i)
			if (reads[i].fallback.engaged)
				why += (why.empty() ? "" : ", ") + reads[i].reason;
		return why.empty() ? FallbackCatalog::Released("product.source_geometry")
		                   : FallbackCatalog::ProductFallback("product.source_geometry", why);
	}

	// The status line of a consumer under a read: its fallback wording while the classical form stands in, "" otherwise.
	inline std::string StatusLine(Consumer consumer, const ReadResult& r)
	{
		return r.fallback.engaged ? std::string(Name(consumer)) + ": classical form (source geometry: " + CauseWord(r.cause) + ")"
		                          : std::string();
	}
}
