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
#include <string>

namespace SourceGeometry
{
	// Bump on ANY change of the record layout or of the law behind a field; core/ProductCatalog.h holds the expected number.
	constexpr uint32_t kUnitVersion = 1;

	using VpSkeleton::ProductId;
	constexpr ProductId kProduct = VpSkeleton::kSourceGeometry;

	// The record is a fixed little-endian layout of at most this many bytes (the registry declares exactly this figure, system RAM).
	constexpr size_t kRecordBytesMax = VpSkeleton::kSourceGeometryRecordBytes;
	static_assert(kRecordBytesMax == 512, "doc 78 section 6: the record is at most 512 B");

	// Stale after maxAgeUs = kMaxAgeIntervals x the longest evaluation interval (doc 78 section 6). The interval is MEASURED by the writer
	// (never a fixed rate): the writer states the result in its record, a reader only compares.
	constexpr int64_t kMaxAgeIntervals = 2;
	inline int64_t MaxAgeUs(int64_t evaluationIntervalMaxUs)
	{
		return evaluationIntervalMaxUs > 0 ? kMaxAgeIntervals * evaluationIntervalMaxUs : 0;
	}

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
		int64_t maxAgeUs = 0;       // stale after this age; <= 0 = the writer stated none: the record is treated as stale (fail closed)
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
	enum : unsigned { kCauseAbsent = 1u, kCauseUnitVersion = 2u, kCauseStale = 4u, kCauseNotYet = 8u, kCauseNoProducer = 16u };

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
		if (e->maxAgeUs <= 0 || nowUs - e->timeUs > e->maxAgeUs || nowUs < e->timeUs - e->maxAgeUs)
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
